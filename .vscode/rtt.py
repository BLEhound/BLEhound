#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Cross-platform RTT log viewer (nRF54LM20A).

Why not tools/rtt.sh: it needs `nc`, and on Windows VSCode's `bash -c` is often WSL
(no J-Link USB access). This script starts JLink.exe's RTT telnet and reads it over a
socket, no nc, native on Windows.

Usage:
  python .vscode/rtt.py                 # stream (Ctrl-C to quit)
  python .vscode/rtt.py --seconds 15    # capture 15s then exit (scripted self-test)
  python .vscode/rtt.py --reset         # reset first, read from the first boot line
  python .vscode/rtt.py --sn 600107328  # pick a J-Link by serial
  env overrides: JLINKEXE / SN / DEVICE
"""
import os
import sys
import time
import socket
import shutil
import platform
import subprocess
import tempfile
import argparse
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass


def find_jlink():
    env = os.environ.get("JLINKEXE")
    if env and Path(env).exists():
        return env
    if platform.system() == "Windows":
        names, dirs = ["JLink.exe"], ["C:/Program Files/SEGGER/JLink",
                                      "C:/Program Files (x86)/SEGGER/JLink"]
    elif platform.system() == "Darwin":
        names, dirs = ["JLinkExe"], ["/Applications/SEGGER/JLink"]
    else:
        names, dirs = ["JLinkExe"], ["/opt/SEGGER/JLink"]
    for d in dirs:
        for n in names:
            p = Path(d) / n
            if p.exists():
                return str(p)
    return shutil.which(names[0]) or names[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sn", default=os.environ.get("SN"))
    ap.add_argument("--device", default=os.environ.get("DEVICE", "NRF54LM20A_M33"))
    ap.add_argument("--port", type=int, default=19021)
    ap.add_argument("--seconds", type=int, default=0, help="0 = stream until Ctrl-C")
    ap.add_argument("--reset", action="store_true")
    a = ap.parse_args()

    jlink = find_jlink()
    attach = 4
    run = ("sleep %d\nr\ng" % ((attach + 2) * 1000)) if a.reset else "g"
    # The RTT control block is in RAM (from 0x20000000); widen the search range to cover it.
    script = ("si SWD\nspeed 4000\ndevice %s\nconnect\n"
              "exec SetRTTSearchRanges = 0x20000000 0x40000\n%s\nsleep 86400000\nq\n" % (a.device, run))
    fd, sp = tempfile.mkstemp(suffix=".jlink")
    os.write(fd, script.encode())
    os.close(fd)
    logf = sp + ".log"
    lf = open(logf, "wb")
    cmd = [jlink, "-RTTTelnetPort", str(a.port)]
    if a.sn:
        cmd += ["-SelectEmuBySN", str(a.sn)]
    cmd += ["-CommanderScript", sp]
    print("== JLINK : %s   device=%s   SN=%s" % (jlink, a.device, a.sn or "(only one online)"))
    p = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT)

    ready = False
    for _ in range(40):
        time.sleep(0.5)
        try:
            txt = open(logf, "r", errors="replace").read()
            if "identified" in txt or "Cortex-M" in txt:
                ready = True
                break
        except Exception:
            pass
        if p.poll() is not None:
            break
    if not ready:
        print("Error: J-Link did not connect. JLinkExe output:")
        try:
            print("\n".join(open(logf, "r", errors="replace").read().splitlines()[-15:]))
        except Exception:
            pass
        p.terminate()
        return 1
    time.sleep(attach)  # wait for the RTT control-block scan

    print("== RTT connected (port=%d), Ctrl-C to quit ==" % a.port)
    rc = 0
    try:
        s = socket.create_connection(("127.0.0.1", a.port), timeout=5)
        s.settimeout(1.0)
        t0 = time.time()
        while True:
            if a.seconds and (time.time() - t0) >= a.seconds:
                break
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                continue
            except Exception:
                break
            if not chunk:
                break
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
        s.close()
    except KeyboardInterrupt:
        pass
    except Exception as e:
        print("telnet error:", e)
        rc = 1
    finally:
        try:
            p.terminate()
        except Exception:
            pass
        try:
            os.unlink(sp)
            os.unlink(logf)
        except Exception:
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
