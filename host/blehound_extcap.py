#!/usr/bin/env python3
"""Wireshark extcap plugin — feeds the BLE packets captured by the BLEhound firmware into Wireshark.

How it works: on startup Wireshark repeatedly calls this script with different
arguments, first asking "which interfaces exist" and "which config options exist",
and only then actually starting the capture. What we do is: open the sniffer's USB
serial port, split frames on 0x00, COBS-decode, convert to PCAP and write into the
fifo Wireshark gives us.

Install (macOS / Linux):

    mkdir -p ~/.config/wireshark/extcap
    cp blehound_extcap.py ~/.config/wireshark/extcap/
    chmod +x ~/.config/wireshark/extcap/blehound_extcap.py

Then restart Wireshark and "BLEhound Sniffer" will appear in the interface list.

Dependency: pyserial (pip3 install pyserial).
"""

import argparse
import os
import queue
import struct
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover - only triggered when the dependency is missing
    sys.stderr.write("pyserial is missing; run first: pip3 install pyserial\n")
    sys.exit(1)

# Three-way aggregation logic (SyncClock/Aggregator). Lives next to this script; the cwd is
# unpredictable when Wireshark calls us, so add the script directory to sys.path before importing.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from blehound_tri_aggregator import Aggregator, FollowRelay, guard_channel_for_board   # noqa: E402

# The value for the aggregated interface — distinct from the per-serial-device paths
TRI_AGGREGATED_IFACE = "tri-aggregated"


# The USB identity the firmware declares in prj.conf
SNIFFER_VID = 0x1915
SNIFFER_PID = 0x520F
SNIFFER_PRODUCT = "BLEhound Sniffer"

# ---- Keep in sync with the firmware's host_iface.h ----
HOST_FRAME_PACKET = 0x01
HOST_CMD_SET_CHANNEL = 0x81
HOST_CMD_SET_TARGET = 0x84
HOST_CMD_SET_HOPPING = 0x85
HOST_CMD_SET_SINGLE_TARGET = 0x87   # 1 byte: 1=single-target mode (default), 0=multi-target evaluation mode
HOST_CMD_FOLLOW = 0x86
FRAME_HEADER_LEN = 17
FLAG_CRC_OK = 1 << 0
HOST_FLAG_TRI = 1 << 3       # tri-board mode: 5 extra bytes after the fixed header (board_id + sync_epoch)
TRI_EXT_LEN = 5

# PCAP: LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR
DLT_BLUETOOTH_LE_LL_WITH_PHDR = 256

# flags bits of the BTLE_RF pseudo-header (see Wireshark packet-btle.c)
RF_FLAG_DEWHITENED = 0x0001
RF_FLAG_SIGNAL_VALID = 0x0002
RF_FLAG_REF_AA_VALID = 0x0010
RF_FLAG_CRC_CHECKED = 0x0400
RF_FLAG_CRC_VALID = 0x0800

ADV_CHANNELS = [37, 38, 39]
DEFAULT_CHANNEL = 37

# Primary-channel legacy advertising PDU types (low 4 bits of the header). AdvA is at payload offset 0 (pdu[2:8]).
ADV_IND = 0x0             # connectable scannable undirected
ADV_DIRECT_IND = 0x1     # connectable directed
ADV_NONCONN_IND = 0x2    # non-connectable non-scannable
SCAN_RSP = 0x4           # scan response (often carries the name)
ADV_SCAN_IND = 0x6       # scannable non-connectable
ADV_EXT_IND = 0x7        # extended advertising: AdvA lives in the ext header, not parsed by the scan
# Types whose payload starts with a 6-byte AdvA, usable for device discovery
_ADV_WITH_ADVA = frozenset({ADV_IND, ADV_DIRECT_IND, ADV_NONCONN_IND, SCAN_RSP, ADV_SCAN_IND})
# Types that carry AdvData (may contain a local name); ADV_DIRECT_IND only has TargetA, no AdvData
_ADV_WITH_DATA = frozenset({ADV_IND, ADV_NONCONN_IND, ADV_SCAN_IND, SCAN_RSP})
ADV_PDU_TYPE_MASK = 0x0F
ADV_TXADD_RANDOM = 0x40  # header bit6: TxAdd, set = random address
SCAN_DEFAULT_SECS = 4.0  # how long one reload scan runs


# ------------------------------------------------------------------ COBS

def cobs_decode(data: bytes) -> bytes:
    """COBS decode. Mirrors the firmware's cobs.c; returns empty on invalid input."""
    out = bytearray()
    i = 0
    n = len(data)

    while i < n:
        code = data[i]
        if code == 0:
            return b""      # a 0 must never appear inside the encoded stream
        i += 1

        end = i + code - 1
        if end > n:
            return b""      # data was truncated
        out += data[i:end]
        i = end

        if code != 0xFF and i < n:
            out.append(0)

    return bytes(out)


# ------------------------------------------------------------------ PCAP

def pcap_global_header() -> bytes:
    return struct.pack(
        "<IHHiIII",
        0xA1B2C3D4,     # magic
        2, 4,           # version
        0,              # timezone
        0,              # timestamp accuracy
        65535,          # snaplen
        DLT_BLUETOOTH_LE_LL_WITH_PHDR,
    )


def pcap_record(ts_epoch_us: int, payload: bytes) -> bytes:
    header = struct.pack("<IIII", ts_epoch_us // 1_000_000, ts_epoch_us % 1_000_000,
                         len(payload), len(payload))
    return header + payload


class TimestampMapper:
    """Maps the firmware's µs counter onto host wall-clock time.

    The firmware counter starts at power-on, so using it directly as the PCAP timestamp
    would show up as 1970 in Wireshark. Here we reconstruct real time as
    "capture-start instant + relative offset", giving both the correct date and the
    firmware's µs-level precision (host-side serial-read jitter is at the millisecond
    level, unsuitable to use directly as the packet arrival time).

    The counter is 32-bit, 1MHz, wrapping roughly every 71.6 minutes, so we accumulate
    the wrap count.
    """

    WRAP = 1 << 32

    def __init__(self, host_epoch_us: int):
        self._host_epoch_us = host_epoch_us
        self._first_fw_us = None
        self._prev_fw_us = 0
        self._wraps = 0

    def to_epoch_us(self, fw_us: int) -> int:
        if self._first_fw_us is None:
            self._first_fw_us = fw_us
            self._prev_fw_us = fw_us

        # a timestamp going backwards can only be a counter wrap (packets are processed in order)
        if fw_us < self._prev_fw_us:
            self._wraps += 1
        self._prev_fw_us = fw_us

        elapsed = fw_us + self._wraps * self.WRAP - self._first_fw_us
        return self._host_epoch_us + elapsed

    def to_epoch_us_mono(self, mono_us: int) -> int:
        """Aggregated path: the input is the aggregator's monotonic 64-bit tick with wraps already
        unrolled (key64); do only a linear mapping and **no longer guess wraps**.
        The aggregated output occasionally has late packets beyond the reorder window (a tick slightly
        earlier than the previous one) — that is real time, not a wrap; using to_epoch_us would treat
        every step-back as a wrap and add 4295s (the cause of the aggregated-pcap timestamp jumps on real
        hardware on 2026-09-19)."""
        if self._first_fw_us is None:
            self._first_fw_us = mono_us
        return self._host_epoch_us + (mono_us - self._first_fw_us)


def ble_channel_to_rf_channel(ble_channel: int) -> int:
    """BLE logical channel index (0..39) → physical RF channel number.

    ⚠️ These two numberings are **not the same thing** (a trap we hit): the rf_channel field in the
    BTLE_RF pseudo-header is the physical channel number (frequency = 2402 + 2n MHz), whereas the
    firmware uses the BLE logical channel index (advertising channels 37/38/39 sit at the lowest,
    middle and highest ends of the spectrum respectively).
    Putting 37 straight in makes Wireshark compute 2476MHz / data channel 35, so all advertising
    PDUs get parsed as "Unknown".
    """
    if ble_channel == 37:
        return 0        # 2402 MHz
    if ble_channel == 38:
        return 12       # 2426 MHz
    if ble_channel == 39:
        return 39       # 2480 MHz
    # data channels 0..10 sit between 37 and 38, and 11..36 between 38 and 39
    return ble_channel + 1 if ble_channel <= 10 else ble_channel + 2


def btle_rf_frame(channel: int, rssi: int, access_addr: int, pdu: bytes,
                  crc: int, crc_ok: bool, phy: int = 0) -> bytes:
    """Assemble the BTLE_RF pseudo-header + the complete link-layer frame.

    The link-layer frame Wireshark expects is access address + PDU + CRC concatenated
    directly, which is also why the firmware does not split off the PDU header and send
    it separately.
    """
    flags = (RF_FLAG_DEWHITENED | RF_FLAG_SIGNAL_VALID |
             RF_FLAG_REF_AA_VALID | RF_FLAG_CRC_CHECKED)
    if crc_ok:
        flags |= RF_FLAG_CRC_VALID

    # the top 2 bits (bit14-15) of the btle_rf pseudo-header flags are the PHY: 0=1M 1=2M 2=Coded.
    # the firmware's phy field is 0=1M 1=2M 2/3=Coded, mapped to 0/1/2.
    flags |= (min(phy, 2) & 0x3) << 14

    phdr = struct.pack(
        "<BbbBIH",
        ble_channel_to_rf_channel(channel),   # physical RF channel number, not the BLE channel index
        rssi,           # signal strength dBm
        0,              # noise level: hardware does not measure it, fill 0 and leave the valid bit unset
        0,              # access address bit-error count: not tracked
        access_addr,
        flags,
    )

    # an LE Coded frame carries 1 extra Coding Indicator byte after the AA (0=S8, 1=S2), which Wireshark
    # reads per the PHY bits in the phdr; without this byte the whole Coded frame gets misaligned and parsed
    # as Malformed. 1M/2M have no CI.
    # firmware phy: 2=Coded S8, 3=Coded S2.
    ci = b""
    if phy == 2:
        ci = b"\x00"
    elif phy == 3:
        ci = b"\x01"

    return phdr + struct.pack("<I", access_addr) + ci + pdu + crc.to_bytes(3, "little")


# ------------------------------------------------------------ serial-port discovery

def find_sniffers():
    """Returns [(serial device path, display name), ...]"""
    found = []
    for port in serial.tools.list_ports.comports():
        if port.vid == SNIFFER_VID and port.pid == SNIFFER_PID:
            found.append((port.device, port.description or SNIFFER_PRODUCT))
    return found


# -------------------------------------------------- scan to pick a target (used by reload)

def _adv_addr_kind(adva: bytes, tx_random: bool) -> str:
    """Classify an address from AdvA. For random ones, the top 2 bits tell RPA / static / non-resolvable apart."""
    if not tx_random:
        return "public"
    top = adva[5] >> 6      # adva is little-endian, the MSB is the last byte
    if top == 0b01:
        return "RPA"        # resolvable private -- rotates periodically, pick then start soon
    if top == 0b11:
        return "static"     # static random -- relatively stable
    return "nrpa"           # non-resolvable private


def _parse_adv_local_name(adv_data: bytes):
    """Pull the Complete (0x09) / Shortened (0x08) Local Name out of AdvData; None if absent."""
    i, n = 0, len(adv_data)
    while i + 1 < n:
        length = adv_data[i]
        if length == 0:
            break
        ad_type = adv_data[i + 1]
        val = adv_data[i + 2:i + 1 + length]
        if ad_type in (0x08, 0x09):
            try:
                return val.decode("utf-8").rstrip("\x00")
            except UnicodeDecodeError:
                return val.decode("latin-1", "replace").rstrip("\x00")
        i += 1 + length
    return None


def _collect_adv(pdu: bytes, rssi: int, devices: dict):
    """Merge one advertising PDU into devices (key = display address string)."""
    if len(pdu) < 8:
        return
    ptype = pdu[0] & ADV_PDU_TYPE_MASK
    if ptype not in _ADV_WITH_ADVA:
        return    # extended advertising etc.: AdvA is not at this offset, skip
    adva = pdu[2:8]
    addr_str = ":".join(f"{b:02X}" for b in reversed(adva))
    atype = _adv_addr_kind(adva, bool(pdu[0] & ADV_TXADD_RANDOM))
    connectable = ptype in (ADV_IND, ADV_DIRECT_IND)
    name = None
    if ptype in _ADV_WITH_DATA:
        end = 2 + pdu[1] if len(pdu) >= 2 + pdu[1] else len(pdu)
        name = _parse_adv_local_name(pdu[8:end])

    d = devices.get(addr_str)
    if d is None:
        devices[addr_str] = {"name": name, "rssi": rssi,
                             "atype": atype, "conn": connectable}
        return
    if rssi > d["rssi"]:
        d["rssi"] = rssi
    if name and not d["name"]:
        d["name"] = name        # the name may arrive later in a SCAN_RSP
    if connectable:
        d["conn"] = True


def scan_devices(interface: str, duration: float = SCAN_DEFAULT_SECS):
    """Briefly scan nearby advertising and return a deduped device list (strongest RSSI first).

    Only legacy advertising is parsed (AdvA at payload offset 0); extended advertising (ADV_EXT_IND)
    is not parsed here. A passive sniffer never sends SCAN_REQ, so devices that only put their name in
    a SCAN_RSP may show up without a name.
    """
    try:
        ser = serial.Serial(interface, timeout=0.2)
    except serial.SerialException as exc:
        sys.stderr.write(f"scan could not open serial port {interface}: {exc}\n")
        return []

    devices: dict = {}
    try:
        ser.dtr = True                                        # the firmware only sends frames once DTR is set
        send_cmd(ser, bytes([HOST_CMD_SET_HOPPING, 1]))       # round-robin 37/38/39
        send_cmd(ser, bytes([HOST_CMD_SET_TARGET]) + bytes(6))  # no filter
        send_cmd(ser, bytes([HOST_CMD_SET_SINGLE_TARGET, 0]))   # don't lock onto a connection while scanning
        time.sleep(0.05)
        ser.reset_input_buffer()

        buf = bytearray()
        deadline = time.time() + duration
        while time.time() < deadline:
            try:
                chunk = ser.read(4096)
            except serial.SerialException:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\x00" in buf:
                frame, _, rest = buf.partition(b"\x00")
                buf = bytearray(rest)
                if not frame:
                    continue
                try:
                    pkt = parse_frame(cobs_decode(bytes(frame)))
                except Exception:
                    continue
                if pkt is None or not pkt["crc_ok"]:
                    continue
                _collect_adv(pkt["pdu"], pkt["rssi"], devices)
    finally:
        try:
            ser.close()
        except Exception:
            pass

    result = [{"addr": a, **v} for a, v in devices.items()]
    result.sort(key=lambda d: d["rssi"], reverse=True)
    return result


def _extcap_sanitize(text: str) -> str:
    """extcap display/value fields must not contain braces or newlines; strip them."""
    return text.replace("{", "(").replace("}", ")").replace("\n", " ").replace("\r", " ")


# How many devices to list in the dropdown (a saturated band can yield hundreds; listing all is unusable, so take the strongest by RSSI)
PICK_MAX_DEVICES = 60


def _print_pick_target_values(interface, arg_number=2, duration=SCAN_DEFAULT_SECS):
    """Scan nearby and print **connectable** devices as pick-target dropdown values (used by both config prefill and reload).

    Only connectable devices are listed (only they send CONNECT_IND and can ever be a follow target), sorted
    by RSSI strongest-first, capped at PICK_MAX_DEVICES; non-connectable beacons/random addresses never enter
    the dropdown.
    """
    scan_iface = interface
    if interface == TRI_AGGREGATED_IFACE or not interface:
        sniffers = find_sniffers()          # the aggregated interface has no single serial port; scan on the first sniffer
        scan_iface = sniffers[0][0] if sniffers else None

    # the empty option always comes first: don't lock, follow the first connection (or fall back to the manual MAC)
    print(f"value {{arg={arg_number}}}{{value=}}{{display=(don't lock, follow the first connection)}}")
    if not scan_iface:
        sys.stderr.write("no sniffer found, cannot scan\n")
        return

    all_devs = scan_devices(scan_iface, duration)
    connectable = [d for d in all_devs if d["conn"]]
    shown = connectable[:PICK_MAX_DEVICES]
    sys.stderr.write(
        f"scan done: {len(all_devs)} advertisers total, {len(connectable)} connectable, "
        f"listing the strongest {len(shown)}\n")

    for d in shown:
        name = d["name"] or "(no name)"
        disp = _extcap_sanitize(
            f"{name} · {d['addr']} · {d['rssi']}dBm · {d['atype']}")
        print(f"value {{arg={arg_number}}}{{value={d['addr']}}}{{display={disp}}}")


def extcap_reload_pick_target(interface, arg_number=2):
    """reload callback: rescan when the refresh circle is clicked (uses a longer duration for a fuller list)."""
    _print_pick_target_values(interface, arg_number, duration=SCAN_DEFAULT_SECS)


# ---------------------------------------------------------- extcap interface

def extcap_interfaces():
    print("extcap {version=1.0}{help=https://github.com/BLEhound/BLEhound}"
          "{display=BLEhound Sniffer}")
    # aggregated interface: read all sniffers at once, align them on a common time base, dedup, and merge into one PCAP (design §6)
    print(f"interface {{value={TRI_AGGREGATED_IFACE}}}"
          "{display=BLEhound Sniffer (3ch aggregated)}")
    # per-serial-device: for single-board / single-channel debugging
    for device, _desc in find_sniffers():
        print(f"interface {{value={device}}}{{display=BLEhound Sniffer ({device})}}")


def extcap_dlts():
    print(f"dlt {{number={DLT_BLUETOOTH_LE_LL_WITH_PHDR}}}"
          "{name=BLUETOOTH_LE_LL_WITH_PHDR}{display=Bluetooth LE LL}")


def extcap_config(interface=None):
    print("arg {number=0}{call=--scan-hopping}{display=Three-channel round-robin scan}"
          "{type=boolflag}{default=true}"
          "{tooltip=Round-robin scan across 37/38/39; turn it off to stay only on the single channel specified below}")

    print(f"arg {{number=1}}{{call=--channel}}{{display=Fixed scan channel}}"
          f"{{type=selector}}{{default={DEFAULT_CHANNEL}}}"
          "{tooltip=The advertising channel to stay on when round-robin is off}")
    for c in ADV_CHANNELS:
        print(f"value {{arg=1}}{{value={c}}}{{display={c}}}")

    # Scan to pick a target: opening this window auto-scans and prefills the dropdown; the circle button rescans. A pick here wins over the manual MAC below.
    print("arg {number=2}{call=--pick-target}{display=Scan for a target}"
          "{type=selector}{reload=true}"
          "{tooltip=Opening this window auto-scans nearby advertisers and fills the dropdown; click the circle to rescan. "
          "Pick the device to follow; this wins over the manual MAC below. "
          "RPA addresses rotate, so after picking start capture and trigger a fresh connection soon.}")
    # scan once when the options open (shorter, to keep the dialog snappy); the circle reload rescans with a longer window
    _print_pick_target_values(interface, arg_number=2, duration=2.5)

    print("arg {number=3}{call=--target}{display=Follow target MAC only (manual)}"
          "{type=string}{default=}"
          "{tooltip=Like AA:BB:CC:DD:EE:FF, follow only connections to that device; used only when the dropdown above is empty; both empty = follow any connection}")

    print("arg {number=4}{call=--include-crc-errors}{display=Include CRC-error packets}"
          "{type=boolflag}{default=false}"
          "{tooltip=When on, packets that fail the CRC check are sent to Wireshark too, useful for diagnosing weak signals}")

    print("arg {number=5}{call=--follow-relay}{display=Tri-board joint follow (aggregated)}"
          "{type=boolflag}{default=false}"
          "{tooltip=Aggregated interface only: when one board captures a CONNECT_IND, relay the connection parameters to the other two to follow together, "
          "so when one board misses packets the other two fill in. No effect on post-encryption updates.}")

    print("arg {number=6}{call=--multi-target}{display=Multi-target mode (evaluation)}"
          "{type=boolflag}{default=false}"
          "{tooltip=Single-target by default: after following one connection it stops scanning and accepts no new connections until it ends; when a target MAC is set "
          "advertising too forwards only packets related to that MAC. Check it to restore multi-target evaluation behavior (scan while idle, follow up to 6 at once).}")


# ---------------------------------------------------------------- capture

def parse_frame(raw: bytes):
    """Parse one already-COBS-decoded frame, returning a dict; returns None if it is not a capture frame."""
    if len(raw) < FRAME_HEADER_LEN or raw[0] != HOST_FRAME_PACKET:
        return None

    flags = raw[1]
    ts_us = int.from_bytes(raw[2:6], "little")
    channel = raw[6]
    rssi = struct.unpack("<b", raw[7:8])[0]
    phy = raw[8]
    access_addr = int.from_bytes(raw[9:13], "little")
    crc = int.from_bytes(raw[13:16], "little")
    pdu_len = raw[16]

    # tri-board extension: when HOST_FLAG_TRI is set, the fixed header is first followed by 5 bytes of
    # board_id + sync_epoch, with pdu shifted back accordingly; old single-board frames (bit unset) still
    # have pdu at offset FRAME_HEADER_LEN — backward compatible.
    board_id = 0
    sync_epoch = 0
    off = FRAME_HEADER_LEN
    if flags & HOST_FLAG_TRI:
        if len(raw) < FRAME_HEADER_LEN + TRI_EXT_LEN:
            return None
        board_id = raw[FRAME_HEADER_LEN]
        sync_epoch = int.from_bytes(raw[FRAME_HEADER_LEN + 1:FRAME_HEADER_LEN + TRI_EXT_LEN],
                                    "little")
        off = FRAME_HEADER_LEN + TRI_EXT_LEN

    pdu = raw[off:off + pdu_len]
    if len(pdu) != pdu_len:
        return None     # length mismatch, discard

    return {
        "ts_us": ts_us,
        "channel": channel,
        "rssi": rssi,
        "phy": phy,
        "access_addr": access_addr,
        "crc": crc,
        "crc_ok": bool(flags & FLAG_CRC_OK),
        "board_id": board_id,
        "sync_epoch": sync_epoch,
        "pdu": pdu,
    }


def parse_mac(text: str):
    """Parse 'AA:BB:CC:DD:EE:FF' into 6 bytes little-endian (matching the air/firmware order); returns None if invalid."""
    text = text.strip()
    if not text:
        return None
    parts = text.replace("-", ":").split(":")
    if len(parts) != 6:
        return None
    try:
        # display order is most-significant-first while the air order is little-endian, so reverse
        return bytes(int(p, 16) for p in reversed(parts))
    except ValueError:
        return None


def send_cmd(ser, payload: bytes):
    ser.write(cobs_encode(payload) + b"\x00")
    ser.flush()


# Flush the fifo at most once every 100ms. The old version flushed on every packet, which at high packet
# rates (measured ~700 packets/sec on three-channel round-robin) would overwhelm the pipe on the Wireshark
# side; throttling makes both throughput and latency steadier.
FLUSH_INTERVAL_S = 0.1


def open_and_configure(interface, channel, include_crc_errors, scan_hopping, target,
                       multi_target=False):
    """Open the sniffer serial port and push the capture configuration. Used to reconnect after glitches/re-enumeration."""
    ser = serial.Serial(interface, timeout=0.2)
    # the firmware uses DTR to tell that "the host has opened the serial port" (host_iface_connected), and
    # sends no data until it is set. Wireshark sets DTR automatically; using pyserial directly we must set it
    # explicitly, otherwise not a single packet is received.
    ser.dtr = True

    # push the capture configuration (commands are COBS-framed too)
    send_cmd(ser, bytes([HOST_CMD_SET_HOPPING, 1 if scan_hopping else 0]))
    if not scan_hopping:
        send_cmd(ser, bytes([HOST_CMD_SET_CHANNEL, channel]))

    mac = parse_mac(target)
    # target MAC: set it if given, otherwise clear the filter (all zeros)
    send_cmd(ser, bytes([HOST_CMD_SET_TARGET]) + (mac or bytes(6)))
    send_cmd(ser, bytes([HOST_CMD_SET_SINGLE_TARGET, 0 if multi_target else 1]))
    # the firmware starts sending frames the moment it sees DTR; frames already in the send buffer before the
    # config above takes effect are unfiltered (observed: after setting a target MAC the first pcap frame is
    # still a different device). Wait for them to drain, then clear.
    time.sleep(0.05)
    ser.reset_input_buffer()
    return ser


def capture(interface, fifo, channel, include_crc_errors, scan_hopping, target,
            multi_target=False):
    """Continuously read frames, convert to PCAP and write into Wireshark's fifo.

    Robustness (following the Nordic sniffer's approach; the old version here was a bare while True with
    zero fault tolerance — the moment Wireshark stopped reading the pipe or the serial port glitched, the
    whole process silently exited, i.e. "captures for a while then stops"):
      - the fifo write end closes (Wireshark stops capturing / closes the window) → clean exit, no crash;
      - serial SerialException (USB glitch / re-enumeration) → close, reopen, re-send config, and continue;
      - a single-frame parse error → drop just that frame, don't take down the whole stream;
      - flush is throttled by FLUSH_INTERVAL_S to avoid overwhelming Wireshark at high packet rates.
    """
    ser = None
    out = None
    try:
        out = open(fifo, "wb")
        out.write(pcap_global_header())
        out.flush()

        mapper = TimestampMapper(int(time.time() * 1_000_000))
        buf = bytearray()
        last_flush = time.time()

        while True:
            # serial not connected / reconnect after a glitch; if it can't connect, wait a bit and retry, don't exit
            if ser is None:
                try:
                    ser = open_and_configure(interface, channel, include_crc_errors, scan_hopping, target,
                                             multi_target)
                except serial.SerialException:
                    time.sleep(0.5)
                    continue
                buf = bytearray()

            try:
                chunk = ser.read(4096)
            except serial.SerialException:
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                continue

            if chunk:
                buf += chunk
                while b"\x00" in buf:
                    frame, _, rest = buf.partition(b"\x00")
                    buf = bytearray(rest)
                    if not frame:
                        continue

                    try:
                        pkt = parse_frame(cobs_decode(bytes(frame)))
                    except Exception:
                        continue    # a bad frame only drops this one
                    if pkt is None:
                        continue
                    if not pkt["crc_ok"] and not include_crc_errors:
                        continue

                    record = btle_rf_frame(pkt["channel"], pkt["rssi"],
                                           pkt["access_addr"], pkt["pdu"],
                                           pkt["crc"], pkt["crc_ok"], pkt["phy"])
                    out.write(pcap_record(mapper.to_epoch_us(pkt["ts_us"]), record))

            now = time.time()
            if now - last_flush >= FLUSH_INTERVAL_S:
                out.flush()
                last_flush = now

    except (BrokenPipeError, OSError):
        # Wireshark closed the fifo (user stopped capturing / closed the window) → wind down normally, no error
        pass
    except KeyboardInterrupt:
        pass
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        if out is not None:
            try:
                out.close()
            except Exception:
                pass


def _write_aggregated_record(out, mapper, pkt):
    """Write one aligned aggregated packet into the fifo. The timestamp source is key64 (reference-board time base, monotonic tick with wraps unrolled)."""
    record = btle_rf_frame(pkt["channel"], pkt["rssi"], pkt["access_addr"],
                           pkt["pdu"], pkt["crc"], pkt["crc_ok"], pkt["phy"])
    out.write(pcap_record(mapper.to_epoch_us_mono(pkt["key64"]), record))


def _aggregated_reader(device, out_q, stop_evt, include_crc_errors, target, relay=None,
                       multi_target=False):
    """Read thread for one sniffer serial port: configured to "guard its strap channel, no round-robin",
    pushing parsed packets into the queue.

    Difference from single-port capture: first turn off round-robin (SET_HOPPING 0), then, based on the
    board_id returned by the first frame, send SET_CHANNEL once to pin that board back to its guard channel
    (0→37 1→38 2→39). We can't trust just the firmware's power-on self-identification: after the firmware has
    followed a connection, or the user previously enabled round-robin via a single-port interface, the guard
    channel may already have drifted (measured on real boards on 2026-09-19: the three ended up guarding
    38/37/38). Auto-reconnect on glitch/re-enumeration, and pin again after reconnecting.
    """
    ser = None
    buf = bytearray()
    mac = parse_mac(target)
    while not stop_evt.is_set():
        if ser is None:
            try:
                ser = serial.Serial(device, timeout=0.2)
                ser.dtr = True
                send_cmd(ser, bytes([HOST_CMD_SET_HOPPING, 0]))
                send_cmd(ser, bytes([HOST_CMD_SET_TARGET]) + (mac or bytes(6)))
                send_cmd(ser, bytes([HOST_CMD_SET_SINGLE_TARGET, 0 if multi_target else 1]))
                time.sleep(0.05)
                ser.reset_input_buffer()   # drop unfiltered frames sent before the config took effect (same as open_and_configure)
                buf = bytearray()
                pinned = False   # guard channel not yet pinned by board_id (board_id is unknown until the first frame)
                my_board = None  # this board's board_id (registered to relay once the first frame determines it)
            except serial.SerialException:
                time.sleep(0.5)
                continue
        try:
            chunk = ser.read(4096)
        except serial.SerialException:
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            continue

        if chunk:
            buf += chunk
            while b"\x00" in buf:
                frame, _, rest = buf.partition(b"\x00")
                buf = bytearray(rest)
                if not frame:
                    continue
                try:
                    pkt = parse_frame(cobs_decode(bytes(frame)))
                except Exception:
                    continue
                if pkt is None:
                    continue
                pkt["host_t"] = time.time()   # host arrival time: the aggregator uses it to pair the two boards' reports of the same SYNC edge
                if not pinned:
                    guard = guard_channel_for_board(pkt["board_id"])
                    if guard is not None:
                        send_cmd(ser, bytes([HOST_CMD_SET_CHANNEL, guard]))
                    pinned = True
                if relay is not None:
                    my_board = pkt["board_id"]
                    relay.register(my_board, ser)                 # register this board's serial port (overwritten after reconnect)
                    relay.observe(my_board, pkt["sync_epoch"], pkt["host_t"])
                    if pkt["crc_ok"]:
                        relay.maybe_relay(my_board, pkt)          # captured a CONNECT_IND → relay it to the other two boards
                if not pkt["crc_ok"] and not include_crc_errors:
                    continue
                out_q.put(pkt)

    if ser is not None:
        try:
            ser.close()
        except Exception:
            pass


def capture_aggregated(fifo, include_crc_errors, target, follow_relay=False,
                       multi_target=False):
    """Three-way aggregated capture: enumerate all sniffers, start one read thread each, and write the
    merged stream into Wireshark's fifo.

    Robustness as with single-port: a board disconnecting/re-enumerating affects only that path and
    auto-reconnects without taking down the whole; wind down cleanly when Wireshark closes the fifo. Don't
    crash when no sniffer is enumerated — wait for the user to plug one in (each read thread retries).
    """
    devices = [d for d, _ in find_sniffers()]
    out_q = queue.Queue(maxsize=10000)
    stop_evt = threading.Event()
    # tri-board joint follow: on capturing a CONNECT_IND, relay it to the other two boards. The target MAC
    # filtering is likewise handled by the relay — the firmware's target filter only blocks connections it
    # captured itself, not the ones injected via the relay.
    relay = FollowRelay(send_cmd, target=parse_mac(target)) if follow_relay else None
    threads = [threading.Thread(target=_aggregated_reader,
                                args=(d, out_q, stop_evt, include_crc_errors, target, relay,
                                      multi_target),
                                daemon=True)
               for d in devices]
    for t in threads:
        t.start()

    agg = Aggregator()
    out = None
    try:
        out = open(fifo, "wb")
        out.write(pcap_global_header())
        out.flush()

        mapper = TimestampMapper(int(time.time() * 1_000_000))
        last_flush = time.time()

        while True:
            try:
                pkt = out_q.get(timeout=0.2)
                for rec in agg.add(pkt):
                    _write_aggregated_record(out, mapper, rec)
            except queue.Empty:
                pass

            now = time.time()
            if now - last_flush >= FLUSH_INTERVAL_S:
                out.flush()
                last_flush = now

    except (BrokenPipeError, OSError):
        pass
    except KeyboardInterrupt:
        pass
    finally:
        stop_evt.set()
        if out is not None:
            try:
                for rec in agg.flush():
                    _write_aggregated_record(out, mapper, rec)
                out.flush()
            except Exception:
                pass
            try:
                out.close()
            except Exception:
                pass


def cobs_encode(data: bytes) -> bytes:
    """COBS encode, used to send commands. Mirrors the firmware's cobs.c."""
    out = bytearray()
    chunk = bytearray()

    for byte in data:
        if byte == 0:
            out.append(len(chunk) + 1)
            out += chunk
            chunk = bytearray()
        else:
            chunk.append(byte)
            if len(chunk) == 254:
                out.append(0xFF)
                out += chunk
                chunk = bytearray()

    out.append(len(chunk) + 1)
    out += chunk
    return bytes(out)


# ----------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description="BLEhound Sniffer extcap plugin",
                                     add_help=False)
    parser.add_argument("--extcap-interfaces", action="store_true")
    parser.add_argument("--extcap-dlts", action="store_true")
    parser.add_argument("--extcap-config", action="store_true")
    parser.add_argument("--extcap-version", nargs="?")
    parser.add_argument("--extcap-interface")
    parser.add_argument("--extcap-reload-option", default=None)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--fifo")
    parser.add_argument("--channel", type=int, default=DEFAULT_CHANNEL)
    parser.add_argument("--scan-hopping", action="store_true")
    parser.add_argument("--target", default="")
    parser.add_argument("--pick-target", default="")
    parser.add_argument("--include-crc-errors", action="store_true")
    parser.add_argument("--follow-relay", action="store_true")
    parser.add_argument("--multi-target", action="store_true")
    parser.add_argument("-h", "--help", action="help")

    args = parser.parse_args()

    if args.extcap_interfaces:
        extcap_interfaces()
        return 0

    if args.extcap_dlts:
        extcap_dlts()
        return 0

    if args.extcap_config:
        # the dropdown's refresh circle was clicked → scan and return only that dropdown's values; otherwise print the full config
        if args.extcap_reload_option:
            extcap_reload_pick_target(args.extcap_interface)
        else:
            extcap_config(args.extcap_interface)
        return 0

    if args.capture:
        if not args.extcap_interface or not args.fifo:
            sys.stderr.write("--capture requires both --extcap-interface and --fifo\n")
            return 1
        # the dropdown pick wins over the manual MAC; both empty = no filter
        target = args.pick_target or args.target
        try:
            if args.extcap_interface == TRI_AGGREGATED_IFACE:
                capture_aggregated(args.fifo, args.include_crc_errors, target,
                                   follow_relay=args.follow_relay,
                                   multi_target=args.multi_target)
            else:
                capture(args.extcap_interface, args.fifo, args.channel,
                        args.include_crc_errors, args.scan_hopping, target,
                        multi_target=args.multi_target)
        except KeyboardInterrupt:
            pass
        except (BrokenPipeError, OSError):
            pass    # Wireshark closes the fifo when it stops capturing; this is a normal exit
        return 0

    parser.print_help(sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
