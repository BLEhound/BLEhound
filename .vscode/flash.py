#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Cross-platform native J-Link flashing (nRF54LM20A).

Why not tools/flash.sh: on Windows VSCode's `bash -c` is often WSL bash, which can't
reach the J-Link USB. This script calls JLink.exe (JLinkExe on mac/linux) directly:
connect -> erase -> program -> reset. The command-line equivalent is tools/flash_jlink.sh.

Usage:
  python .vscode/flash.py [v1|v2]              # flash <build>/blehound/blehound_merged.hex to the only J-Link online
  python .vscode/flash.py v1 --sn 600107328    # pick a J-Link by serial when several are attached
  python .vscode/flash.py v1 --recover         # mass-erase (unlock APPROTECT) then flash
  python .vscode/flash.py v1 --recover-only    # only erase/unlock, do not flash
  env overrides: JLINKEXE / SN / DEVICE / SPEED
"""
import os
import sys
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

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
PRODUCTS = HERE / "zephyr-products.yaml"


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


def product_build_dir(product):
    try:
        import yaml
        prods = yaml.safe_load(PRODUCTS.read_text(encoding="utf-8")) or {}
        key = product if product in prods else (
            "blehound_" + product if ("blehound_" + product) in prods else None)
        if key:
            return prods[key].get("build", "build")
    except Exception:
        pass
    return "build_v2" if product in ("v2", "blehound_v2") else "build"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("product", nargs="?", default="v1")
    ap.add_argument("--sn", default=os.environ.get("SN"))
    ap.add_argument("--device", default=os.environ.get("DEVICE", "NRF54LM20A_M33"))
    ap.add_argument("--speed", default=os.environ.get("SPEED", "4000"))
    ap.add_argument("--hex", default=None)
    ap.add_argument("--recover", action="store_true")
    ap.add_argument("--recover-only", dest="recover_only", action="store_true")
    a = ap.parse_args()

    build = product_build_dir(a.product)
    hexf = a.hex or str(REPO / build / "blehound" / "blehound_merged.hex")
    if not a.recover_only and not Path(hexf).exists():
        sys.exit("Error: firmware not found: %s (build first: python .vscode/zephyr-build.py %s)" % (hexf, a.product))

    jlink = find_jlink()
    lines = ["si SWD", "speed %s" % a.speed, "device %s" % a.device, "connect"]
    # nRF unlock happens automatically at connect; erase does a whole-chip mass-erase.
    if a.recover or a.recover_only:
        lines += ["erase"]
    if not a.recover_only:
        lines += ["erase", 'loadfile "%s"' % hexf, "r", "g"]
    lines += ["q", ""]

    fd, sp = tempfile.mkstemp(suffix=".jlink")
    os.write(fd, "\n".join(lines).encode())
    os.close(fd)

    cmd = [jlink, "-ExitOnError", "1"]
    if a.sn:
        cmd += ["-SelectEmuBySN", str(a.sn)]
    cmd += ["-CommanderScript", sp]

    print("== JLINK : %s" % jlink)
    print("== DEVICE: %s   SN: %s" % (a.device, a.sn or "(only one online)"))
    print("== HEX   : %s" % (hexf if not a.recover_only else "(erase only)"))
    rc = 1
    try:
        rc = subprocess.run(cmd, timeout=150).returncode
    except subprocess.TimeoutExpired:
        print("Timeout: J-Link stuck. With several probes online pass --sn <serial>; "
              "otherwise re-plug the J-Link USB and retry.")
    finally:
        try:
            os.unlink(sp)
        except Exception:
            pass
    print("== done" if rc == 0 else "== failed/incomplete rc=%d" % rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())
