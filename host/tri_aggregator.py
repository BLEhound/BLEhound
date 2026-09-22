#!/usr/bin/env python3
"""Pure-logic module for three-dongle aggregation (no I/O, easy to unit-test on the host).

Responsibilities (design doc/2026-08-13-… §6):
  1. SyncClock — using each board's reported sync_epoch (the tick of the same SYNC edge in each
     board's TIMER), align the three timestamp streams to the "reference board" (board 0) time base;
  2. Aggregator — merge the three parsed packet streams by aligned time base, dedup by an (AA,CRC,PDU)
     window, and emit a single time-consistent, duplicate-free, ordered stream.

The extcap main script (nrf_sniffer_extcap.py) does the actual multi-serial-port reading and feeds the
parsed dicts to this module; this module never touches serial/files and can self-test its pure logic with
`--selftest`.

Time-base alignment principle: the three boards' free-running 32-bit µs counters are not synchronized.
board 0 raises a SYNC edge periodically (~1Hz); all three capture the tick of that same physical edge on
their own clocks and put the "most recent tick" into each packet's sync_epoch field. Then
offset[b] = sync_epoch[b] - sync_epoch[ref], and subtracting offset from any board's tick converts it to
the reference-board time base. With no SYNC observations it degrades to comparing each board's own ts
directly (the §6 transitional form).

Usage (self-test): python3 tri_aggregator.py --selftest
"""

import os
import sys
import threading
import time
from typing import Optional   # avoid `float | None`: Wireshark runs this file with the system python3 (3.9 on macOS)

MASK32 = 0xFFFFFFFF
HALF32 = 0x80000000
WRAP = 1 << 32


# board_id → the advertising channel that board must guard (matches the firmware board_role.c strap table: 0→37 1→38 2→39).
# The aggregated mode uses it to pull a drifted board back to its guard channel: after the firmware has followed
# a connection the guard channel may have been rotated away, and extcap can't trust just the firmware's power-on
# self-identification, so it re-sets it by board_id every time the serial port is opened.
GUARD_CHANNEL_BY_BOARD = {0: 37, 1: 38, 2: 39}


def guard_channel_for_board(board_id: int):
    """Return the guard channel for board_id; ids outside 0..2 return None (send nothing, keep the firmware's current state)."""
    return GUARD_CHANNEL_BY_BOARD.get(board_id)


def _sdiff32(a: int, b: int) -> int:
    """32-bit circular signed difference a-b (handles wraparound)."""
    return ((a - b + HALF32) & MASK32) - HALF32


class SyncClock:
    """Align timestamps to the reference-board (board 0) time base using each board's reported sync_epoch.

    Pairing rule: the two boards each keep one tick for the **same physical edge**, and offset = that board's
    tick − the reference board's tick. The two boards' reports arrive at the host over their own serial ports
    and may differ by hundreds of ms; if you naively take "each board's latest one", when one board's new edge
    arrives first it gets paired with the other board's previous edge, offsetting by one heartbeat period
    (≈1s). So carry the host arrival time: for the same edge the two boards' arrivals differ by < pair_window_s,
    whereas a mispair differs by ≥ one heartbeat period. With no arrival time (host_t=None) it degrades to
    "latest against latest".
    """

    HIST = 4   # how many recent edges to keep per board

    def __init__(self, ref_board: int = 0, pair_window_s: float = 0.5):
        self._ref = ref_board
        self._pair_window = pair_window_s
        self._hist: dict[int, list[tuple[int, Optional[float]]]] = {}   # board -> [(tick, host_t)]
        self._offset: dict[int, int] = {}

    def observe(self, board_id: int, local_tick: int, host_t: Optional[float] = None) -> None:
        """Record a board's most recent SYNC edge tick (0 means none yet, ignored) and its host arrival time."""
        if not local_tick:
            return
        h = self._hist.setdefault(board_id, [])
        if h and h[-1][0] == (local_tick & MASK32):
            return                                   # same edge reported again (every frame carries the epoch)
        h.append((local_tick & MASK32, host_t))
        del h[:-self.HIST]
        boards = [b for b in self._hist if b != self._ref] if board_id == self._ref else [board_id]
        for b in boards:
            self._pair(b)

    def _pair(self, board: int) -> None:
        ref_h = self._hist.get(self._ref, [])
        for tick_b, t_b in reversed(self._hist.get(board, [])):
            for tick_r, t_r in reversed(ref_h):
                if t_b is None or t_r is None or abs(t_b - t_r) <= self._pair_window:
                    self._offset[board] = (tick_b - tick_r) & MASK32
                    return

    def offset(self, board_id: int):
        """This board's clock offset relative to the reference board (32-bit unsigned); returns None when undetermined."""
        if board_id == self._ref:
            return 0
        return self._offset.get(board_id)

    def to_ref_tick(self, board_id: int, local_tick: int):
        """Convert a board's tick to the reference-board time base (32-bit); returns None when it can't be aligned."""
        off = self.offset(board_id)
        if off is None:
            return None
        return (local_tick - off) & MASK32


class FollowRelay:
    """Tri-board joint-follow relay: when one board captures a CONNECT_IND, convert the connection parameters to
    the other two boards' local time bases via the SYNC inter-board offset and send them via HOST_CMD_FOLLOW, so
    all three follow the same connection in parallel (the aggregator then dedups by AA/PDU).

    Benefit: when one board loses the connection due to dropped packets / a missed PHY update / a lost one-shot
    control packet, the other two (independent antenna positions and noise) may still be on it, so the
    aggregated stream stays unbroken. No benefit for post-encryption updates (all three equally fail to decrypt
    the ciphertext, so the failures are correlated).

    Threading model: each read thread registers its own serial port via register(), calls observe() every frame
    to feed the SYNC time base, and calls maybe_relay() on capturing a CONNECT_IND; locking is internal.
    send_cmd(ser, payload) is injected by the constructor (extcap passes send_cmd, which does COBS framing).

    Target MAC filtering: when target (6 bytes air little-endian, matching what HOST_CMD_SET_TARGET sends to the
    firmware) is non-empty, relay only CONNECT_INDs whose AdvA equals target. The firmware's target filter only
    blocks connections "it captured itself"; relayed-injected connections don't pass through it — without
    filtering here, the other two boards would be fed non-target connections to follow.
    """

    ADV_AA = 0x8E89BED6
    CONNECT_IND = 0x05
    HOST_CMD_FOLLOW = 0x86
    UNIT_US = 1250
    TX_WIN_DELAY_US = 1250
    AIR_US_PER_BYTE = 8
    RELAY_TTL_S = 5.0        # how long not to re-relay the same AA (relay a given connection only once)

    def __init__(self, send_cmd, ref_board: int = 0, target=None):
        self._send_cmd = send_cmd
        self._target = bytes(target) if target else None   # 6 bytes air little-endian; None = no filtering
        self.clock = SyncClock(ref_board)
        self._ser: dict = {}          # board_id -> serial
        self._relayed: dict = {}      # aa -> host_t of the most recent relay
        self._lock = threading.Lock()
        self.stats = {"relayed": 0, "sent_cmds": 0, "skipped_no_offset": 0, "skipped_dup": 0,
                      "skipped_target": 0}

    def register(self, board_id: int, ser) -> None:
        with self._lock:
            self._ser[board_id] = ser

    def observe(self, board_id: int, sync_epoch: int, host_t) -> None:
        with self._lock:
            self.clock.observe(board_id, sync_epoch, host_t)

    @staticmethod
    def is_connect_ind(pkt: dict) -> bool:
        pdu = pkt.get("pdu", b"")
        # on secondary channels (0..36) the same type is AUX_CONNECT_REQ: txWinDelay and PHY both differ, the relay formula doesn't apply, so don't relay
        return (pkt.get("access_addr") == FollowRelay.ADV_AA and len(pdu) >= 2 + 34
                and (pdu[0] & 0x0F) == FollowRelay.CONNECT_IND
                and pkt.get("channel", 37) >= 37)

    @staticmethod
    def parse_connect_ind(pdu: bytes) -> dict:
        ll = pdu[2 + 12:2 + 34]      # skip header/len(2) + InitA(6) + AdvA(6)

        def g16(o):
            return ll[o] | (ll[o + 1] << 8)

        return {
            "aa": int.from_bytes(ll[0:4], "little"),
            "crc_init": ll[4] | (ll[5] << 8) | (ll[6] << 16),
            "win_size": ll[7],
            "win_offset": g16(8),
            "interval": g16(10),
            "latency": g16(12),
            "timeout": g16(14),
            "chan_map": bytes(ll[16:21]),
            "hop": ll[21] & 0x1F,
            "csa2": 1 if (pdu[0] & 0x20) else 0,
            "adva": bytes(pdu[2 + 6:2 + 12]),   # little-endian AdvA (for target filtering/diagnostics)
        }

    def build_follow_cmd(self, ci: dict, anchor0_us: int) -> bytes:
        """Assemble the 25-byte HOST_CMD_FOLLOW parameters (unframed; framing is done by send_cmd)."""
        b = bytearray([self.HOST_CMD_FOLLOW])
        b += int(ci["aa"]).to_bytes(4, "little")
        b += int(ci["crc_init"]).to_bytes(3, "little")
        b += ci["chan_map"]
        b += bytes([ci["hop"] & 0x1F, 1 if ci["csa2"] else 0])
        b += int(ci["interval"]).to_bytes(2, "little")
        b += int(ci["latency"]).to_bytes(2, "little")
        b += int(ci["timeout"]).to_bytes(2, "little")
        b += bytes([ci["win_size"] & 0xFF])
        b += (int(anchor0_us) & MASK32).to_bytes(4, "little")
        return bytes(b)

    def anchor0_for(self, from_board: int, ts_us: int, pdu_len: int,
                    win_offset: int, to_board: int):
        """The event0 anchor of the CONNECT_IND captured by from_board, converted to to_board's local us. Returns None if the offset is unknown."""
        air = (2 + pdu_len + 3) * self.AIR_US_PER_BYTE
        anchor_from = (ts_us + air + self.TX_WIN_DELAY_US +
                       win_offset * self.UNIT_US) & MASK32
        ref = self.clock.to_ref_tick(from_board, anchor_from)
        off_to = self.clock.offset(to_board)
        if ref is None or off_to is None:
            return None
        return (ref + off_to) & MASK32

    def maybe_relay(self, from_board: int, pkt: dict) -> list:
        """Called on capturing a CONNECT_IND: send FOLLOW to the other two boards. Returns the list of board_ids actually sent to."""
        if not self.is_connect_ind(pkt):
            return []
        ci = self.parse_connect_ind(pkt["pdu"])
        if self._target is not None and ci["adva"] != self._target:
            with self._lock:
                self.stats["skipped_target"] += 1
            return []   # connection to a non-target device: don't relay, and don't occupy the dedup table
        host_t = pkt.get("host_t") or time.time()
        with self._lock:
            last = self._relayed.get(ci["aa"])
            if last is not None and host_t - last < self.RELAY_TTL_S:
                self.stats["skipped_dup"] += 1
                return []
            self._relayed[ci["aa"]] = host_t
            targets = [(b, ser) for b, ser in self._ser.items() if b != from_board]
        sent = []
        for b, ser in targets:
            anchor = self.anchor0_for(from_board, pkt["ts_us"], pkt["pdu"][1],
                                      ci["win_offset"], b)
            if anchor is None:
                with self._lock:
                    self.stats["skipped_no_offset"] += 1
                continue
            try:
                self._send_cmd(ser, self.build_follow_cmd(ci, anchor))
                sent.append(b)
                with self._lock:
                    self.stats["sent_cmds"] += 1
            except Exception:
                pass
        if sent:
            with self._lock:
                self.stats["relayed"] += 1
        return sent


class Aggregator:
    """Three-way aggregation: align time base → dedup → ordered output.

    - dedup_window_us: when two boards capture the same air packet, their aligned times should differ within
      this window; dedup by (AA,CRC,PDU). Set to 100µs: across boards the same aligned packet measures ~75µs
      apart (SYNC offset error), whereas the central/peripheral empty packets within the same event (same key,
      one T_IFS≈150µs apart) must stay **separate** — the window must be < T_IFS, otherwise the two real
      central/peripheral empty packets get merged into one;
    - reorder_window_us: the depth of the out-of-order buffer before output. After a packet arrives, only
      buffered packets earlier than it by more than reorder_window are safe to emit (no earlier packet can
      still be inserted ahead of them).
    """

    # how long at most to hold packets while the time base isn't established (measured by the board's own ticks): board0 heartbeat is 1Hz, so if none arrives within 3s treat it as single-board/broken-link and degrade to release
    PENDING_TIMEOUT_US = 3_000_000

    def __init__(self, dedup_window_us: int = 100, reorder_window_us: int = 300_000,
                 ref_board: int = 0):
        """reorder_window_us defaults to 300ms: the three serial ports are read by their own threads, and the
        arrival skew from USB/firmware send buffering measured up to the hundred-ms level; too small a window
        would emit late-arriving early packets immediately and make the output go backwards (on 2026-09-19 real hardware a 5ms window caused the aggregated pcap timestamps to jump)."""
        self.clock = SyncClock(ref_board)
        self._dedup_us = dedup_window_us
        self._reorder_us = reorder_window_us
        self._buf: list[dict] = []          # records in the buffer (carrying key64)
        self._emitted: dict[tuple, int] = {}  # (aa,crc,pdu) -> aligned tick of the most recent **emitted** record (output-side dedup)
        self._pending: dict[int, list[dict]] = {}       # boards whose time base isn't established yet: packets held back
        self._pending_first: dict[int, int] = {}        # the raw tick of that board's first held packet (for timeout)
        # 32-bit aligned tick → monotonic 64-bit (for sorting/mapping)
        self._prev_v = None
        self._base = 0
        self._max_key64 = None

    def _to64(self, v: int) -> int:
        """Convert a 32-bit aligned tick to a monotonic 64-bit value. Only a "big drop" (>2^31) is judged a wrap."""
        if self._prev_v is None:
            self._prev_v = v
            return v
        if v < self._prev_v and (self._prev_v - v) > HALF32:
            self._base += WRAP
        # _prev_v tracks the largest value seen, to avoid a small out-of-order step-back falsely triggering a wrap
        if v > self._prev_v or (self._prev_v - v) > HALF32:
            self._prev_v = v
        return self._base + v

    def add(self, pkt: dict) -> list[dict]:
        """Feed a parsed frame from one stream, returning the ordered, deduped packets safe to emit right now (possibly empty).

        Boards whose time base isn't established (offset unknown) have their packets held back: once a raw tick
        leaks into the output, the whole stream jumps by one inter-board offset the moment alignment takes effect
        (254s on real hardware), and downstream 32-bit wrap handling adds a further 4295s. As soon as an offset
        appears, the held packets are re-added by aligned tick; if there's still no SYNC after PENDING_TIMEOUT_US
        (single-board/broken-link) it degrades to releasing by raw tick, so they aren't held forever.
        """
        board = pkt.get("board_id", 0)
        self.clock.observe(board, pkt.get("sync_epoch", 0), pkt.get("host_t"))

        if self.clock.offset(board) is None:
            q = self._pending.setdefault(board, [])
            q.append(pkt)
            first = self._pending_first.setdefault(board, pkt["ts_us"])
            if _sdiff32(pkt["ts_us"], first) < self.PENDING_TIMEOUT_US:
                return []
            held = self._pending.pop(board)
            self._pending_first.pop(board, None)
            out = []
            for p in held:
                out += self._ingest(p, p["ts_us"] & MASK32)   # degraded: coarse-sort by each board's own ts
            return out

        out = []
        for p in self._pending.pop(board, []):
            out += self._ingest(p, self.clock.to_ref_tick(board, p["ts_us"]))
        self._pending_first.pop(board, None)
        out += self._ingest(pkt, self.clock.to_ref_tick(board, pkt["ts_us"]))
        return out

    def _ingest(self, pkt: dict, aligned: int) -> list[dict]:
        """A packet whose aligned tick is already computed: unroll to 64-bit → enter the reorder buffer → emit those that are due.
        Dedup is not done here: the three read threads arrive with tens of ms of skew, so deduping by arrival
        order would miss some (a late-arriving copy can't find its earlier one). It's moved into _release,
        operating on the **sorted** buffer, where copies sit adjacent, independent of arrival order."""
        rec = dict(pkt)
        rec["aligned"] = aligned
        rec["key64"] = self._to64(aligned)
        self._buf.append(rec)
        if self._max_key64 is None or rec["key64"] > self._max_key64:
            self._max_key64 = rec["key64"]

        return self._release(final=False)

    def _dedup_emit(self, records: list) -> list:
        """Dedup on the to-be-emitted records already sorted by key64: records with the same (aa,crc,pdu) whose
        aligned tick is within the dedup window of the previous **emitted** record with the same key are treated
        as the same air packet captured by multiple boards, keeping only the first one out."""
        out = []
        for r in records:
            k = (r["access_addr"], r["crc"], bytes(r["pdu"]))
            prev = self._emitted.get(k)
            if prev is not None and abs(_sdiff32(r["aligned"], prev)) <= self._dedup_us:
                continue                      # another board's copy of the same packet, drop
            self._emitted[k] = r["aligned"]
            out.append(r)
        # clear bookkeeping far earlier than the reorder window to avoid unbounded growth (using the newest sorted tick as reference)
        if records:
            newest = records[-1]["aligned"]
            horizon = self._reorder_us + self._dedup_us * 8
            stale = [k for k, t in self._emitted.items() if _sdiff32(newest, t) > horizon]
            for k in stale:
                del self._emitted[k]
        return out

    def _release(self, final: bool) -> list[dict]:
        if not self._buf:
            return []
        self._buf.sort(key=lambda r: r["key64"])
        if final:
            out, self._buf = self._buf, []
            return self._dedup_emit(out)

        cutoff = self._max_key64 - self._reorder_us
        out = [r for r in self._buf if r["key64"] <= cutoff]
        self._buf = [r for r in self._buf if r["key64"] > cutoff]
        return self._dedup_emit(out)

    def flush(self) -> list[dict]:
        """Wind down: first degrade-release any packets still held per board, then emit everything left in the buffer (ordered)."""
        for board, held in list(self._pending.items()):
            for p in held:
                self._ingest(p, self.clock.to_ref_tick(board, p["ts_us"]) if self.clock.offset(board) is not None
                             else p["ts_us"] & MASK32)
        self._pending.clear()
        self._pending_first.clear()
        return self._release(final=True)


# ------------------------------------------------------------------ self-test

def _mk_pkt(board_id, ts_us, sync_epoch, aa, crc, pdu):
    return {
        "board_id": board_id, "ts_us": ts_us, "sync_epoch": sync_epoch,
        "access_addr": aa, "crc": crc, "pdu": pdu,
        "channel": 37, "rssi": -50, "phy": 0, "crc_ok": True,
    }


def _selftest_frame_ext():
    """Verify parse_frame correctly extracts the board_id / sync_epoch of a HOST_FLAG_TRI extended frame."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from nrf_sniffer_extcap import parse_frame, HOST_FRAME_PACKET, HOST_FLAG_TRI

    hdr = bytes([HOST_FRAME_PACKET, HOST_FLAG_TRI | 1])
    hdr += (1234).to_bytes(4, "little")
    hdr += bytes([37, (256 - 40) & 0xFF, 0])
    hdr += (0x8E89BED6).to_bytes(4, "little")
    hdr += (0x555555).to_bytes(3, "little")
    hdr += bytes([2])
    ext = bytes([2]) + (99999).to_bytes(4, "little")
    pdu = bytes([0x00, 0x00])

    pkt = parse_frame(hdr + ext + pdu)
    assert pkt is not None, "extended-frame parse failed"
    assert pkt["board_id"] == 2, pkt
    assert pkt["sync_epoch"] == 99999, pkt
    assert pkt["pdu"] == pdu, pkt
    assert pkt["crc_ok"] is True, pkt

    old = bytes([HOST_FRAME_PACKET, 1]) + (1).to_bytes(4, "little")
    old += bytes([38, (256 - 50) & 0xFF, 0]) + (0x8E89BED6).to_bytes(4, "little")
    old += (0x555555).to_bytes(3, "little") + bytes([2]) + pdu
    pkt2 = parse_frame(old)
    assert pkt2 is not None and pkt2["board_id"] == 0 and pkt2["sync_epoch"] == 0, pkt2

    print("frame-ext selftest OK")


def _selftest_syncclock():
    c = SyncClock(ref_board=0)
    c.observe(0, 1000)
    c.observe(1, 4000)     # board1 is 3000 ahead of board0
    assert c.offset(1) == 3000, c.offset(1)
    # board1's tick 4500 → reference board 1500
    assert c.to_ref_tick(1, 4500) == 1500, c.to_ref_tick(1, 4500)
    # the reference board has no offset from itself
    assert c.to_ref_tick(0, 1234) == 1234
    # an unobserved board can't be aligned
    assert c.to_ref_tick(2, 10) is None
    # wraparound: board1's sync still computes correctly after the counter wraps
    c2 = SyncClock(0)
    c2.observe(0, 100)
    c2.observe(1, 50)      # offset = (50-100) mod 2^32
    assert c2.to_ref_tick(1, 60) == 110, c2.to_ref_tick(1, 60)
    print("syncclock selftest OK")


def _selftest_aggregate():
    agg = Aggregator(dedup_window_us=200, reorder_window_us=5000)
    # both boards see the same SYNC edge: board0 tick=1000, board1 tick=4000 → offset 3000
    # the same air packet is captured by both boards: board0 ts=2000 (aligned 2000), board1 ts=5000 (aligned 2000)
    pdu = bytes([0x02, 0x06, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66])
    out = []
    out += agg.add(_mk_pkt(0, 2000, 1000, 0xAABBCCDD, 0x123456, pdu))
    out += agg.add(_mk_pkt(1, 5000, 4000, 0xAABBCCDD, 0x123456, pdu))  # should be deduped
    # a later, different packet pushes the earlier ones out of the reorder window
    pdu2 = bytes([0x02, 0x06, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44])
    out += agg.add(_mk_pkt(0, 20000, 1000, 0xAABBCCDD, 0x654321, pdu2))
    out += agg.flush()

    # after dedup only 2 distinct packets should remain, ascending by aligned time base (2000 first, 20000 after)
    assert len(out) == 2, [(o["aligned"], o["crc"]) for o in out]
    assert out[0]["crc"] == 0x123456 and out[1]["crc"] == 0x654321, out
    assert out[0]["aligned"] <= out[1]["aligned"], out

    # even with no sync_epoch (degraded) it should still emit and not crash
    agg2 = Aggregator()
    o2 = agg2.add(_mk_pkt(1, 500, 0, 0x11111111, 0x1, bytes([0, 0])))
    o2 += agg2.flush()
    assert len(o2) == 1, o2
    print("aggregate selftest OK")


def _selftest_syncclock_pairing():
    """The two boards' ticks for the same edge arrive at the host over their own serial ports and may differ by
    hundreds of ms. When board1's new edge arrives before board0's, it must not be paired with board0's previous
    edge — the offset would be off by one heartbeat period (≈1s), which on real hardware shows up as the
    aggregated pcap repeatedly stepping back ±0.7s. Pair by host arrival time: for the same edge the two boards'
    arrivals differ by < 0.5s, whereas a mispair differs by ≥ 1s."""
    c = SyncClock()
    c.observe(0, 1_000_000, host_t=10.00)
    c.observe(1, 4_000_000, host_t=10.20)       # edge k: offset = 3s
    assert c.offset(1) == 3_000_000, c.offset(1)
    c.observe(1, 5_000_000, host_t=11.05)       # edge k+1 arrives from board1 first, board0's hasn't arrived yet
    assert c.offset(1) == 3_000_000, c.offset(1)   # must not pair as 5_000_000-1_000_000 = 4s
    c.observe(0, 2_000_000, host_t=11.30)       # board0's k+1 arrives → still 3s
    assert c.offset(1) == 3_000_000, c.offset(1)
    c.observe(0, 3_000_000, host_t=12.10)       # edge k+2, this time board0 arrives first
    c.observe(1, 6_000_050, host_t=12.35)       # board1 arrives later, with 50µs jitter → use the latest pair
    assert c.offset(1) == 3_000_050, c.offset(1)
    print("syncclock-pairing selftest OK")


def _selftest_hold_until_synced():
    """Before the time base is established, non-reference-board packets must be held and released by aligned tick
    once the offset appears; otherwise a raw tick leaks into the output and the whole stream jumps by one
    inter-board offset the moment alignment takes effect (254s on real hardware), with downstream wrap handling
    adding a further 4295s."""
    agg = Aggregator(dedup_window_us=200, reorder_window_us=5000)
    aa = 0xAABBCCDD
    pdu = bytes([0x02, 0x06, 1, 2, 3, 4, 5, 6])
    out = []
    out += agg.add(_mk_pkt(1, 5000, 0, aa, 0x1, pdu))      # board1 arrives before any SYNC
    assert out == [], "board1's packet must not be emitted before alignment"
    out += agg.add(_mk_pkt(0, 2000, 1000, aa, 0x2, pdu))   # board0 heartbeat: own tick 1000
    out += agg.add(_mk_pkt(1, 6000, 4000, aa, 0x3, pdu))   # board1 same-edge tick 4000 → offset 3000
    out += agg.flush()
    aligned = [o["aligned"] for o in out]
    assert aligned == [2000, 2000, 3000], aligned           # 5000-3000 / 2000 / 6000-3000, and ascending
    assert all(o["key64"] < WRAP for o in out), "no false wrap should appear"

    # never any SYNC (single-board/broken-link): after 3s degrade to releasing by raw tick, must not hold forever
    agg2 = Aggregator()
    o2 = agg2.add(_mk_pkt(1, 100, 0, aa, 0x1, pdu))
    assert o2 == []
    o2 += agg2.add(_mk_pkt(1, 100 + 3_100_000, 0, aa, 0x2, pdu))
    o2 += agg2.flush()
    assert len(o2) == 2, o2
    print("hold-until-synced selftest OK")


def _selftest_reorder_window():
    """The default reorder window must cover the three-way USB arrival jitter (measured up to the hundred-ms level), so late-arriving early packets are still ordered by aligned tick."""
    assert Aggregator()._reorder_us >= 200_000, Aggregator()._reorder_us
    agg = Aggregator(reorder_window_us=300_000)
    aa = 0x11223344
    pdu = bytes([0x02, 0x06, 9, 9, 9, 9, 9, 9])
    out = []
    out += agg.add(_mk_pkt(0, 1_000_000, 1000, aa, 0x1, pdu))
    out += agg.add(_mk_pkt(0, 1_100_000, 1000, aa, 0x2, pdu))
    out += agg.add(_mk_pkt(0, 1_050_000, 1000, aa, 0x3, pdu))   # an earlier packet arriving 50ms late
    assert out == [], out
    out += agg.flush()
    assert [o["aligned"] for o in out] == [1_000_000, 1_050_000, 1_100_000], [o["aligned"] for o in out]
    print("reorder-window selftest OK")


def _selftest_guard_channel():
    """board_id → the advertising channel it must guard (matches the firmware board_role.c); invalid ids return None."""
    assert guard_channel_for_board(0) == 37
    assert guard_channel_for_board(1) == 38
    assert guard_channel_for_board(2) == 39
    assert guard_channel_for_board(3) is None
    assert guard_channel_for_board(255) is None
    print("guard-channel selftest OK")


def _selftest_follow_relay():
    """FollowRelay: parse CONNECT_IND, convert the anchor to the other two boards via the SYNC offset, assemble the command, dedup."""
    sent = []
    relay = FollowRelay(send_cmd=lambda ser, payload: sent.append((ser, payload)))
    relay.register(0, "ser0")
    relay.register(1, "ser1")
    relay.register(2, "ser2")
    # establish inter-board offsets: same SYNC edge, board0=1000, board1=1500 (+500), board2=800 (-200)
    relay.observe(0, 1000, 10.0)
    relay.observe(1, 1500, 10.01)
    relay.observe(2, 800, 10.02)
    assert relay.clock.offset(1) == 500 and relay.clock.offset(2) == (800 - 1000) & MASK32

    # build a CONNECT_IND captured by board0: AdvA=aa.., with LLData specifying aa/interval/winoffset, etc.
    initA = bytes.fromhex("010203040506")
    advA = bytes.fromhex("aabbccddeeff")
    ll = bytearray(22)
    ll[0:4] = (0x12345678).to_bytes(4, "little")   # AA
    ll[4:7] = (0xABCDEF).to_bytes(3, "little")      # crc_init
    ll[7] = 3                                        # win_size
    ll[8:10] = (8).to_bytes(2, "little")            # win_offset=8 (10ms)
    ll[10:12] = (24).to_bytes(2, "little")          # interval=24 (30ms)
    ll[12:14] = (0).to_bytes(2, "little")           # latency
    ll[14:16] = (200).to_bytes(2, "little")         # timeout
    ll[16:21] = bytes([0xFF, 0xFF, 0xFF, 0xFF, 0x1F])  # chan_map all 1s
    ll[21] = 0x05                                     # hop=5
    pdu = bytes([0x05, 34]) + initA + advA + bytes(ll)   # header: type=CONNECT_IND(0x5), CSA2 bit unset
    pkt = {"access_addr": 0x8E89BED6, "pdu": pdu, "ts_us": 100000, "host_t": 10.05}

    assert FollowRelay.is_connect_ind(pkt)
    ci = FollowRelay.parse_connect_ind(pdu)
    assert ci["aa"] == 0x12345678 and ci["crc_init"] == 0xABCDEF
    assert ci["interval"] == 24 and ci["win_offset"] == 8 and ci["hop"] == 5 and ci["csa2"] == 0
    assert ci["adva"] == advA

    out = relay.maybe_relay(0, pkt)
    assert sorted(out) == [1, 2], out
    assert len(sent) == 2
    # verify the anchor sent to board1 = board0's anchor + offset(1) - offset(0)
    air = (2 + 34 + 3) * 8
    anchor0 = (100000 + air + 1250 + 8 * 1250) & MASK32
    exp1 = (anchor0 + 500) & MASK32
    payload1 = dict(sent)["ser1"]
    # payload = cmd(1) + params(25) = 26 bytes; param i is at payload[1+i]
    assert payload1[0] == 0x86 and len(payload1) == 26, (len(payload1), payload1)
    got1 = int.from_bytes(payload1[22:26], "little")    # anchor0 = param offset 21..24
    assert got1 == exp1, (got1, exp1)
    assert int.from_bytes(payload1[1:5], "little") == 0x12345678      # aa
    assert (payload1[15] | (payload1[16] << 8)) == 24                 # interval (param offset 14)

    # don't re-relay the same AA within 5s
    sent.clear()
    assert relay.maybe_relay(0, pkt) == []
    assert relay.stats["skipped_dup"] == 1

    # don't send to a board whose offset is unknown
    relay2 = FollowRelay(send_cmd=lambda ser, payload: sent.append((ser, payload)))
    relay2.register(0, "s0"); relay2.register(1, "s1")
    assert relay2.maybe_relay(0, pkt) == []
    assert relay2.stats["skipped_no_offset"] == 1

    # target MAC filtering: a relay with target set only sends for CONNECT_INDs whose AdvA matches, otherwise the
    # other two boards get injected with non-target connections (the capturing board has firmware filtering, the
    # two relayed boards don't). target and adva are both air little-endian.
    def _relay_with_target(target):
        r = FollowRelay(send_cmd=lambda ser, payload: sent.append((ser, payload)), target=target)
        for b in (0, 1, 2):
            r.register(b, f"s{b}")
        r.observe(0, 1000, 10.0); r.observe(1, 1500, 10.01); r.observe(2, 800, 10.02)
        return r

    sent.clear()
    r_hit = _relay_with_target(advA)
    assert sorted(r_hit.maybe_relay(0, pkt)) == [1, 2]
    assert len(sent) == 2 and r_hit.stats["skipped_target"] == 0

    sent.clear()
    r_miss = _relay_with_target(bytes.fromhex("112233445566"))
    assert r_miss.maybe_relay(0, pkt) == []
    assert sent == [] and r_miss.stats["skipped_target"] == 1
    assert r_miss.stats["skipped_dup"] == 0   # something filtered out by target must not occupy the dedup table
    print("follow-relay selftest OK")


def _selftest_dedup_out_of_order():
    """Three boards' copies of the same packet arrive out of order, with a later event's packet interleaved:
    dedup should still collapse the three into one. This is the real-hardware follow-relay scenario — read
    threads arrive with tens of ms of skew, and the old arrival-side dedup would miss some."""
    agg = Aggregator(reorder_window_us=300_000)
    for b, tick in [(0, 1_000_000), (1, 1_000_050), (2, 999_970)]:
        agg.clock.observe(b, tick, 100.0 + b * 0.001)
    empty = bytes([0x01, 0x00])
    out = []
    # event N: board0 arrives first; event N+1: board0's empty packet (same key, 30ms later) also arrives first;
    # then board1, board2 arrive 40ms late with their copies of event N.
    ref_n = 5_000_000
    out += agg.add(_mk(0, ref_n, empty))                       # N, board0
    out += agg.add(_mk(0, ref_n + 30_000, empty))              # N+1, board0 (same key)
    out += agg.add(_mk(1, ref_n + 50, empty, off=50))          # N, board1 (late)
    out += agg.add(_mk(2, ref_n - 30, empty, off=-30))         # N, board2 (late)
    out += agg.add(_mk(1, ref_n + 30_050, empty, off=50))      # N+1, board1
    out += agg.add(_mk(2, ref_n + 29_970, empty, off=-30))     # N+1, board2
    out += agg.flush()
    # empty packets from two distinct events, each kept once → 2 total
    assert len(out) == 2, [r["aligned"] for r in out]
    print("dedup-out-of-order selftest OK")


def _mk(board, ref_tick, pdu, off=0):
    # local ts = ref + that board's offset; offset(1)=+50, offset(2)=-30 (see the observe calls above)
    return {"board_id": board, "ts_us": (ref_tick + off) & MASK32, "sync_epoch": 1_000_000 + off,
            "access_addr": 0x11223344, "crc": 0xABCDEF, "pdu": pdu, "crc_ok": True,
            "channel": 10, "rssi": -50, "phy": 0, "host_t": 200.0}


def _selftest_btle_rf_coded():
    """Coded PHY frame: LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR requires 1 Coding Indicator byte after the AA
    (0 = S=8, 1 = S=2), which Wireshark's btle parser reads per the PHY bits in the phdr. The old version didn't
    insert this byte, so Coded frames were all parsed as Malformed (2026-09-19 real hardware: after
    LL_PHY_UPDATE_IND switched to Coded, every frame was Malformed).
    1M/2M frames carry no CI and are left as-is."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from nrf_sniffer_extcap import btle_rf_frame

    aa = 0x173E6971
    pdu = bytes([0x01, 0x00])          # Empty PDU
    crc = 0x71D452
    phdr_len = 10
    body_1m = btle_rf_frame(35, -68, aa, pdu, crc, True, phy=0)[phdr_len:]
    assert body_1m == aa.to_bytes(4, "little") + pdu + crc.to_bytes(3, "little"), body_1m.hex()

    body_s8 = btle_rf_frame(35, -68, aa, pdu, crc, True, phy=2)[phdr_len:]
    assert body_s8 == aa.to_bytes(4, "little") + bytes([0x00]) + pdu + crc.to_bytes(3, "little"), \
        body_s8.hex()

    body_s2 = btle_rf_frame(35, -68, aa, pdu, crc, True, phy=3)[phdr_len:]
    assert body_s2 == aa.to_bytes(4, "little") + bytes([0x01]) + pdu + crc.to_bytes(3, "little"), \
        body_s2.hex()

    # the phdr's PHY bits (bit14-15) should be 2 (LE Coded) for both S8/S2
    for phy in (2, 3):
        flags = int.from_bytes(btle_rf_frame(35, -68, aa, pdu, crc, True, phy=phy)[8:10], "little")
        assert (flags >> 14) & 0x3 == 2, hex(flags)
    print("btle-rf-coded selftest OK")


def _selftest():
    _selftest_frame_ext()
    _selftest_syncclock()
    _selftest_aggregate()
    _selftest_syncclock_pairing()
    _selftest_hold_until_synced()
    _selftest_reorder_window()
    _selftest_guard_channel()
    _selftest_follow_relay()
    _selftest_dedup_out_of_order()
    _selftest_btle_rf_coded()


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        _selftest()
    else:
        sys.stderr.write("usage: tri_aggregator.py --selftest\n")
        sys.exit(1)
