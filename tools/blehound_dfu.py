#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
BLEhound USB DFU -- self-contained SMP (mcumgr) serial client, depends only on pyserial.

No nrfutil, no BLEhound-Analyzer CLI: both speak the standard mcumgr protocol (SMP over
serial) to the MCUboot firmware loader; this reimplements the protocol itself in Python,
so the firmware repo can do the update on its own and the open-source repo just ships this file.

Flow (firmware side: firmware/blehound/projects/blehound/sysbuild.conf):
  1. Send HOST_CMD_ENTER_DFU (0x8B) on the app's CDC port; the app writes the boot mode and resets;
  2. MCUboot starts the firmware loader, USB re-enumerates as "BLEhound Loader" (VID 1915 / PID 5210);
  3. SMP uploads the signed app image into image-0, resets back into the app, then waits for the
     app port (PID 520F) and reads a status frame to confirm the version.
  If the board is already in the loader (a previous update was interrupted), update starts at step 3.

Usage:
  blehound_dfu.py ports                       # list serial ports, tag BLEhound app / loader
  blehound_dfu.py update <port>|all [app.bin] # full update (default image build/blehound/blehound_ota.bin)
  blehound_dfu.py enter <port>                # only put the app into the loader
  blehound_dfu.py list <port>                 # list the images in the loader
  blehound_dfu.py upload <port> <app.bin>     # only upload (board must already be in the loader)
  blehound_dfu.py reset <port>                # only reset (board must already be in the loader)
  <port> is /dev/cu.usbmodemXXX on macOS, /dev/ttyACMx on Linux, COMx on Windows.

SMP serial framing (same as Zephyr's mcumgr UART transport):
  raw   = len(2, big-endian, includes CRC) + body + crc16(2, big-endian)   body = SMP header(8) + CBOR
  lines = raw in 93-byte base64 segments; first segment prefixed 0x06 0x09, continuations 0x04 0x14, each line ends '\\n'
  crc16 = CRC-16/XMODEM (poly 0x1021, init 0) over body; the receiver re-runs it over body+crc and expects 0
"""

import base64
import os
import struct
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Error: pyserial required (pip install pyserial)")

VID_NORDIC = 0x1915
PID_APP = 0x520F      # app (shared by the sniffer / NUS test firmware)
PID_LOADER = 0x5210   # MCUboot firmware loader (usb_mcumgr)

HOST_CMD_QUERY_STATUS = 0x88
HOST_CMD_ENTER_DFU = 0x8B
HOST_FRAME_STATUS = 0x02

SMP_MARK_FIRST = b"\x06\x09"
SMP_MARK_CONT = b"\x04\x14"
SMP_FRAME_RAW = 93            # raw bytes per base64 line (124 chars + 2 prefix + '\n' <= 127)
CHUNK_SLACK_BYTES = 24        # extra margin kept under buf_size per chunk (see SmpClient.upload)
FIRST_CHUNK_BYTES = 256       # first-chunk cap (see SmpClient.upload)
SMP_LINE_DELAY_S = float(os.environ.get("BLEHOUND_SMP_LINE_DELAY_MS", "1")) / 1000.0   # inter-line delay
SMP_OP_READ, SMP_OP_READ_RSP, SMP_OP_WRITE, SMP_OP_WRITE_RSP = 0, 1, 2, 3
SMP_GROUP_OS, SMP_GROUP_IMG = 0, 1
SMP_OS_ID_RESET, SMP_OS_ID_MCUMGR_PARAMS = 5, 6
SMP_IMG_ID_STATE, SMP_IMG_ID_UPLOAD = 0, 1

DEFAULT_IMAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "blehound", "blehound_ota.bin")


# ----------------------------------------------------------------------------- utilities

def crc16_xmodem(data, seed=0):
    crc = seed
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def cobs_encode(data):
    out = bytearray([0])
    idx, code = 0, 1
    for b in data:
        if b == 0:
            out[idx] = code
            idx, code = len(out), 1
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[idx] = code
                idx, code = len(out), 1
                out.append(0)
    out[idx] = code
    return bytes(out)


def cobs_decode(data):
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        out += data[i:i + code - 1]
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


# ----------------------------------------------------------------------------- CBOR (minimal subset)

def cbor_encode(obj):
    def head(major, n):
        if n < 24:
            return bytes([major << 5 | n])
        if n < 0x100:
            return bytes([major << 5 | 24, n])
        if n < 0x10000:
            return bytes([major << 5 | 25]) + struct.pack(">H", n)
        if n < 0x100000000:
            return bytes([major << 5 | 26]) + struct.pack(">I", n)
        return bytes([major << 5 | 27]) + struct.pack(">Q", n)

    if obj is True:
        return b"\xf5"
    if obj is False:
        return b"\xf4"
    if obj is None:
        return b"\xf6"
    if isinstance(obj, int):
        return head(0, obj) if obj >= 0 else head(1, -1 - obj)
    if isinstance(obj, bytes):
        return head(2, len(obj)) + obj
    if isinstance(obj, str):
        b = obj.encode()
        return head(3, len(b)) + b
    if isinstance(obj, (list, tuple)):
        return head(4, len(obj)) + b"".join(cbor_encode(x) for x in obj)
    if isinstance(obj, dict):
        return head(5, len(obj)) + b"".join(cbor_encode(k) + cbor_encode(v) for k, v in obj.items())
    raise TypeError("cbor: unsupported %r" % type(obj))


def cbor_decode(data):
    pos = 0

    def take(n):
        nonlocal pos
        if pos + n > len(data):
            raise ValueError("cbor: truncated")
        chunk = data[pos:pos + n]
        pos += n
        return chunk

    def item():
        nonlocal pos
        ib = take(1)[0]
        major, info = ib >> 5, ib & 0x1F
        if info < 24:
            n = info
        elif info == 24:
            n = take(1)[0]
        elif info == 25:
            n = struct.unpack(">H", take(2))[0]
        elif info == 26:
            n = struct.unpack(">I", take(4))[0]
        elif info == 27:
            n = struct.unpack(">Q", take(8))[0]
        elif info == 31:
            n = None   # indefinite length
        else:
            raise ValueError("cbor: bad additional info %d" % info)
        if major == 0:
            return n
        if major == 1:
            return -1 - n
        if major == 2:
            return bytes(take(n))
        if major == 3:
            return take(n).decode(errors="replace")
        if major == 4:
            if n is None:
                out = []
                while data[pos] != 0xFF:
                    out.append(item())
                pos += 1
                return out
            return [item() for _ in range(n)]
        if major == 5:
            out = {}
            if n is None:
                while data[pos] != 0xFF:
                    k = item()
                    out[k] = item()
                pos += 1
                return out
            for _ in range(n):
                k = item()
                out[k] = item()
            return out
        if major == 7:
            if info == 20:
                return False
            if info == 21:
                return True
            if info in (22, 23):
                return None
            if info == 25:
                return struct.unpack(">e", take(2))[0]
            if info == 26:
                return struct.unpack(">f", take(4))[0]
            if info == 27:
                return struct.unpack(">d", take(8))[0]
        raise ValueError("cbor: unsupported major %d/%d" % (major, info))

    return item()


# ----------------------------------------------------------------------------- SMP framing

def smp_encode(op, group, cmd_id, seq, payload):
    body = struct.pack(">BBHHBB", op, 0, len(payload), group, seq, cmd_id) + payload
    raw = struct.pack(">H", len(body) + 2) + body + struct.pack(">H", crc16_xmodem(body))
    lines = bytearray()
    for off in range(0, len(raw), SMP_FRAME_RAW):
        lines += (SMP_MARK_FIRST if off == 0 else SMP_MARK_CONT)
        lines += base64.b64encode(raw[off:off + SMP_FRAME_RAW]) + b"\n"
    return bytes(lines)


class SmpDeframer:
    """Feed serial bytes line by line; returns (hdr dict, payload bytes) for each complete packet."""

    def __init__(self):
        self.line = bytearray()
        self.pkt = bytearray()
        self.expect = None

    def feed(self, data):
        out = []
        for b in data:
            if b != 0x0A:
                self.line.append(b)
                continue
            line = bytes(self.line)
            self.line = bytearray()
            if len(line) < 2:
                continue
            mark, b64 = line[:2], line[2:]
            try:
                raw = base64.b64decode(b64, validate=True)
            except Exception:
                continue
            if mark == SMP_MARK_FIRST:
                self.pkt = bytearray(raw)
                if len(self.pkt) < 2:
                    self.expect = None
                    continue
                self.expect = struct.unpack(">H", self.pkt[:2])[0]
            elif mark == SMP_MARK_CONT and self.expect is not None:
                self.pkt += raw
            else:
                continue
            if self.expect is not None and len(self.pkt) - 2 >= self.expect:
                body_crc = bytes(self.pkt[2:2 + self.expect])
                self.expect = None
                if len(body_crc) < 10 or crc16_xmodem(body_crc) != 0:
                    continue
                body = body_crc[:-2]
                op, flags, plen, group, seq, cmd_id = struct.unpack(">BBHHBB", body[:8])
                out.append(({"op": op, "group": group, "seq": seq, "id": cmd_id}, body[8:8 + plen]))
        return out


class SmpClient:
    def __init__(self, port, timeout=5.0):
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.ser.dtr = True
        self.timeout = timeout
        self.seq = 0
        self.deframer = SmpDeframer()

    def close(self):
        self.ser.close()

    def request(self, op, group, cmd_id, payload_obj, timeout=None):
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFF
        # Send line by line and flush each: writing the whole packet at once makes USB pack several
        # lines into one 64-byte transfer streamed to the loader; on real hardware (2026-10-12) the
        # loader resets its USB when it gets a 1.2 KB upload packet, while line-by-line is stable.
        for line in smp_encode(op, group, cmd_id, seq, cbor_encode(payload_obj)).split(b"\n"):
            if line:
                self.ser.write(line + b"\n")
                self.ser.flush()
                if SMP_LINE_DELAY_S > 0:
                    time.sleep(SMP_LINE_DELAY_S)
        deadline = time.time() + (timeout or self.timeout)
        while time.time() < deadline:
            data = self.ser.read(4096)
            if not data:
                continue
            for hdr, payload in self.deframer.feed(data):
                if hdr["seq"] == seq and hdr["group"] == group and hdr["id"] == cmd_id:
                    rsp = cbor_decode(payload) if payload else {}
                    err = rsp.get("rc", 0) if isinstance(rsp, dict) else 0
                    if isinstance(rsp, dict) and "err" in rsp and isinstance(rsp["err"], dict):
                        err = rsp["err"].get("rc", 0)
                    if err:
                        raise RuntimeError("SMP group %d id %d: rc=%d" % (group, cmd_id, err))
                    return rsp
        raise TimeoutError("SMP group %d id %d: no response" % (group, cmd_id))

    def image_list(self):
        return self.request(SMP_OP_READ, SMP_GROUP_IMG, SMP_IMG_ID_STATE, {}).get("images", [])

    def mcumgr_params(self):
        try:
            return self.request(SMP_OP_READ, SMP_GROUP_OS, SMP_OS_ID_MCUMGR_PARAMS, {}, timeout=2.0)
        except (TimeoutError, RuntimeError):
            return {}

    def reset(self):
        self.request(SMP_OP_WRITE, SMP_GROUP_OS, SMP_OS_ID_RESET, {})

    def upload(self, image, progress=None):
        """Upload image (bytes) in chunks of the loader's buf_size; follow the off in each response."""
        params = self.mcumgr_params()
        buf_size = int(params.get("buf_size", 512))
        off = 0
        t0 = time.time()
        while off < len(image):
            fields = {"image": 0, "off": off}
            if off == 0:
                fields["len"] = len(image)
                # No "sha": on real hardware (2026-10-12, NCS 3.4 usb_mcumgr loader) a first chunk
                # carrying the sha field makes the loader reset its USB on receipt; sha is only for
                # resumable uploads, and a full upload from 0 does not need it.
            # Keep the whole SMP packet (8-byte header + CBOR) under the loader buffer: estimate the cap
            # first, shrink if over. Reserve the worst case (off/len as u32 each, data length u16) plus 16
            # bytes: on real hardware filling the packet to buf_size (chunk 1189) gets no ack, staying
            # under 1178 with the margin is stable.
            overhead = 8 + len(cbor_encode(dict(fields, data=b""))) + 3 + CHUNK_SLACK_BYTES
            chunk = max(32, min(len(image) - off, buf_size - overhead))
            if off == 0:
                # First chunk must be small: on receiving it the loader prepares/erases the image-0 slot
                # and can't keep up with the following lines; on real hardware a 1.2 KB first chunk either
                # gets no ack or resets USB, while 256 bytes gets a response within 0.5 s.
                chunk = min(chunk, FIRST_CHUNK_BYTES)
            while True:
                payload = cbor_encode(dict(fields, data=image[off:off + chunk]))
                if 8 + len(payload) <= buf_size or chunk <= 32:
                    break
                chunk -= 16
            rsp = None
            for attempt in range(2):
                try:
                    rsp = self.request(SMP_OP_WRITE, SMP_GROUP_IMG, SMP_IMG_ID_UPLOAD,
                                       dict(fields, data=image[off:off + chunk]),
                                       timeout=30.0 if off == 0 else 10.0)
                    break
                except TimeoutError:
                    if attempt == 1:
                        raise
            new_off = int(rsp.get("off", off + chunk))
            if new_off <= off:
                raise RuntimeError("upload stalled at offset %d (loader asked for %d)" % (off, new_off))
            off = new_off
            if progress:
                progress(off, len(image))
        return time.time() - t0


# ----------------------------------------------------------------------------- app port (capture protocol)

def find_ports(pid=None):
    found = []
    for p in list_ports.comports():
        if p.vid == VID_NORDIC and (pid is None or p.pid == pid):
            found.append(p)
    return sorted(found, key=lambda p: p.device)


def app_status(port, tries=3):
    """Send QUERY_STATUS and read one status frame; return dict or None (not a sniffer/NUS app port)."""
    try:
        with serial.Serial(port, 115200, timeout=0.3) as s:
            s.dtr = True
            time.sleep(0.5)
            s.reset_input_buffer()
            for _ in range(tries):
                s.write(b"\x00" + cobs_encode(bytes([HOST_CMD_QUERY_STATUS])) + b"\x00")
                s.flush()
                time.sleep(0.5)
            raw = s.read(65536)
    except serial.SerialException:
        return None
    for frame in raw.split(b"\x00"):
        if not frame:
            continue
        d = cobs_decode(frame)
        if len(d) >= 15 and d[0] == HOST_FRAME_STATUS:
            fw_len = d[14]
            return {"board_id": d[2], "guard_channel": d[3],
                    "fw": d[15:15 + fw_len].decode(errors="replace")}
    return None


def enter_loader(port):
    """Put the app into the loader. On real hardware, sending a single ENTER_DFU right after opening the
    port often has no effect (the parser may be stuck mid-frame on a leftover); first let CDC settle,
    send an empty separator + QUERY_STATUS to resync, then send ENTER_DFU twice."""
    with serial.Serial(port, 115200, timeout=0.2) as s:
        s.dtr = True
        time.sleep(0.5)
        s.write(b"\x00" + cobs_encode(bytes([HOST_CMD_QUERY_STATUS])) + b"\x00")
        s.flush()
        time.sleep(0.3)
        s.reset_input_buffer()
        for _ in range(2):
            try:
                s.write(b"\x00" + cobs_encode(bytes([HOST_CMD_ENTER_DFU])) + b"\x00")
                s.flush()
                time.sleep(0.3)
            except serial.SerialException:
                break   # board already reset into the loader, port gone


def wait_ports(pid, count_at_least, timeout_s=15.0):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        ports = find_ports(pid)
        if len(ports) >= count_at_least:
            return ports
        time.sleep(0.5)
    return find_ports(pid)


def is_loader_port(port):
    try:
        c = SmpClient(port, timeout=2.0)
        try:
            c.image_list()
            return True
        finally:
            c.close()
    except Exception:
        return False


# ----------------------------------------------------------------------------- subcommands

def cmd_ports():
    for p in list_ports.comports():
        tag = ""
        if p.vid == VID_NORDIC and p.pid == PID_APP:
            tag = "BLEhound app"
        elif p.vid == VID_NORDIC and p.pid == PID_LOADER:
            tag = "BLEhound loader"
        ids = "%04x:%04x" % (p.vid, p.pid) if p.vid is not None else ""
        print("%-28s %-10s %s" % (p.device, ids, tag))


def cmd_list(port):
    c = SmpClient(port)
    try:
        for img in c.image_list():
            flags = " ".join(k for k in ("active", "confirmed", "pending", "bootable", "permanent") if img.get(k))
            print("slot %s: version %s %s" % (img.get("slot"), img.get("version"), flags))
    finally:
        c.close()


def cmd_upload(port, image_path):
    image = open(image_path, "rb").read()
    c = SmpClient(port)
    try:
        params = c.mcumgr_params()
        print("loader buf_size %s, image %d bytes" % (params.get("buf_size", "?"), len(image)))

        def prog(off, total):
            sys.stdout.write("\r  %d / %d bytes" % (off, total))
            sys.stdout.flush()

        secs = c.upload(image, prog)
        print("\n  uploaded in %.2f s" % secs)
        for img in c.image_list():
            if img.get("slot") == 0:
                print("  slot 0: version %s%s" % (img.get("version"), " confirmed" if img.get("confirmed") else ""))
    finally:
        c.close()


def cmd_reset(port):
    c = SmpClient(port)
    try:
        c.reset()
        print("  reset acknowledged")
    finally:
        c.close()


def cmd_update(port, image_path):
    if not os.path.isfile(image_path):
        sys.exit("Error: image not found: %s" % image_path)
    print("== %s: entering loader" % port)
    if is_loader_port(port):
        print("   already in the loader, skipping the entry step")
        loader = port
    else:
        before = len(find_ports(PID_LOADER))
        enter_loader(port)
        loaders = wait_ports(PID_LOADER, before + 1)
        if len(loaders) <= before:
            sys.exit("Error: %s sent ENTER_DFU but no new loader enumerated (PID %04X)" % (port, PID_LOADER))
        # macOS keeps the port name; Windows/Linux gets a new one, so take the newly appeared port
        loader = port if any(p.device == port for p in loaders) else loaders[-1].device
        time.sleep(1.0)
        print("   loader at %s" % loader)
    print("== %s: uploading %s" % (loader, image_path))
    cmd_upload(loader, image_path)
    print("== %s: resetting back into the app" % loader)
    cmd_reset(loader)
    apps_before = find_ports(PID_APP)
    apps = wait_ports(PID_APP, len(apps_before) + 1 if loader not in [p.device for p in apps_before] else len(apps_before), 15.0)
    app = port if any(p.device == port for p in apps) else (apps[-1].device if apps else None)
    if app is None:
        print("   ⚠️ app port did not come back (image may be rejected by MCUboot, board stays in the loader; check the signing key)")
        return False
    time.sleep(2.0)
    st = app_status(app)
    if st is None:
        print("   ⚠️ %s came back but has no status frame (not sniffer firmware, or still in the loader)" % app)
        return False
    print("== %s: done board %d guard-channel %d firmware %s" % (app, st["board_id"], st["guard_channel"], st["fw"]))
    return True


def cmd_update_all(image_path):
    ports = [p.device for p in find_ports(PID_APP)] + [p.device for p in find_ports(PID_LOADER)]
    if not ports:
        sys.exit("Error: no BLEhound serial port found (VID %04X)" % VID_NORDIC)
    ok = 0
    for p in ports:
        if cmd_update(p, image_path):
            ok += 1
        time.sleep(2.0)
    print("== %d / %d boards updated" % (ok, len(ports)))
    return ok == len(ports)


def selftest():
    """Framing/CBOR self-test (no hardware): encode a packet -> feed it back line by line -> header and payload match."""
    payload = {"image": 0, "off": 1234, "len": 99999, "sha": bytes(range(32)), "data": b"\x00" * 200, "ok": True}
    enc = smp_encode(SMP_OP_WRITE, SMP_GROUP_IMG, SMP_IMG_ID_UPLOAD, 7, cbor_encode(payload))
    assert enc.startswith(SMP_MARK_FIRST) and enc.count(b"\n") >= 3
    got = SmpDeframer().feed(enc)
    assert len(got) == 1, got
    hdr, pl = got[0]
    assert hdr == {"op": SMP_OP_WRITE, "group": SMP_GROUP_IMG, "seq": 7, "id": SMP_IMG_ID_UPLOAD}, hdr
    assert cbor_decode(pl) == payload
    assert crc16_xmodem(b"123456789") == 0x31C3
    assert cobs_decode(cobs_encode(b"\x00\x01\x00")) == b"\x00\x01\x00"
    print("blehound_dfu selftest OK")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd, args = argv[1], argv[2:]
    if cmd == "ports":
        cmd_ports()
    elif cmd == "selftest":
        selftest()
    elif cmd == "enter" and len(args) == 1:
        enter_loader(args[0])
        print("sent ENTER_DFU to %s" % args[0])
    elif cmd == "list" and len(args) == 1:
        cmd_list(args[0])
    elif cmd == "upload" and len(args) == 2:
        cmd_upload(args[0], args[1])
    elif cmd == "reset" and len(args) == 1:
        cmd_reset(args[0])
    elif cmd == "update" and 1 <= len(args) <= 2:
        img = args[1] if len(args) == 2 else os.path.normpath(DEFAULT_IMAGE)
        return 0 if (cmd_update_all(img) if args[0] == "all" else cmd_update(args[0], img)) else 2
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
