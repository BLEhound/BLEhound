#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Cross-platform build entry point (macOS / Windows / Linux).

Products (board target / app dir / SDK / build dir) are registered in the sibling
zephyr-products.yaml — add a board there, no need to touch this script.

Self-contained west workspace:
    workspace root = <repo>/firmware/        (.west lives here)
    app            = firmware/<app>          (yaml 'app' field, relative to the ws root)
    board defs     = firmware/blehound/boards/ (auto-discovered via blehound/zephyr/module.yml)

Usage:
    python .vscode/zephyr-build.py [product] [--pristine | --increment] [-- <extra west/cmake args>]
    product may be the full name (blehound_v1) or the short form (v1); omit for the first one.

Zephyr SDK: if ZEPHYR_SDK_INSTALL_DIR is unset, probe ~/zephyr-sdk-<ver> and
/opt/zephyr-sdk-<ver>; otherwise leave it to west/cmake to locate.
"""
import os
import sys
import shutil
import platform
import subprocess
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("Error: PyYAML required (pip install pyyaml) to read zephyr-products.yaml")

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
WS = REPO / "firmware"                       # west workspace root
PRODUCTS = HERE / "zephyr-products.yaml"


def detect_sdk(sdk_ver):
    v = os.environ.get("ZEPHYR_SDK_INSTALL_DIR")
    if v and Path(v).is_dir():
        return v
    name = "zephyr-sdk-%s" % sdk_ver
    for c in (Path.home() / name, Path("/opt") / name):
        if c.is_dir():
            return str(c)
    return None  # let west/cmake locate the SDK


def resolve_product(products, arg):
    if arg is None:
        return next(iter(products))
    if arg in products:
        return arg
    if ("blehound_" + arg) in products:          # short form v1 -> blehound_v1
        return "blehound_" + arg
    sys.exit("Error: unknown product '%s'. Options: %s" % (arg, ", ".join(products)))


def main():
    products = yaml.safe_load(PRODUCTS.read_text(encoding="utf-8")) or {}
    if not products:
        sys.exit("Error: %s is empty" % PRODUCTS)

    args = sys.argv[1:]
    passthru = []
    if "--" in args:
        i = args.index("--")
        passthru = args[i + 1:]
        args = args[:i]

    increment = "--increment" in args
    pristine = "--pristine" in args
    positional = [a for a in args if not a.startswith("-")]
    key = resolve_product(products, positional[0] if positional else None)

    p = products[key]
    board = p["board"]
    app = WS / p["app"]
    build_dir = REPO / p.get("build", "build")
    sdk_ver = str(p.get("sdk", "1.0.1"))

    # First build (build dir missing) implies pristine: avoids an occasional first-run
    # sysbuild CMakeCache race; --increment is unaffected.
    if not increment and not pristine and not build_dir.exists():
        pristine = True
        print("== first build, enabling pristine")

    if not (WS / ".west").is_dir():
        sys.exit(
            "Error: workspace not initialized (%s/.west missing).\n"
            "       Set up: cd \"%s\" && west init -l blehound && west update && west zephyr-export"
            % (WS, WS))

    env = os.environ.copy()
    sdk = detect_sdk(sdk_ver)
    if sdk:
        env["ZEPHYR_SDK_INSTALL_DIR"] = sdk
    env.setdefault("ZEPHYR_TOOLCHAIN_VARIANT", "zephyr")

    # Signing key (optional): use it if present, else fall back to MCUboot's dev key
    # (local debugging only, do not release). Create one with tools/keygen.sh.
    cmake_args = list(passthru)
    skey = os.environ.get("BLEHOUND_SIGNING_KEY") or str(Path.home() / ".blehound" / "keys" / "blehound_ed25519.pem")
    if Path(skey).is_file():
        cmake_args.append('-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="%s"' % Path(skey).as_posix())
        print("== SIGN KEY : %s" % skey)
    else:
        print("WARN  no signing key, using MCUboot's development key (local debug only). Create one: tools/keygen.sh")

    west = shutil.which("west") or "west"
    cmd = [west, "build", "-d", str(build_dir)]
    if not increment:
        cmd += ["-b", board, "-s", str(app), "--sysbuild"]
        if pristine:
            cmd += ["-p", "always"]
    if cmake_args:
        cmd += ["--"] + cmake_args

    print("== PRODUCT: %s" % key)
    print("== WS     : %s" % WS)
    print("== SDK    : %s" % (sdk or "(not found; left to west/cmake)"))
    print("== BOARD  : %s" % board)
    print("== APP    : %s" % app)
    print("== BUILD  : %s" % build_dir)
    print("== CMD    : %s" % " ".join(cmd))
    return subprocess.call(cmd, cwd=str(WS), env=env)


if __name__ == "__main__":
    sys.exit(main())
