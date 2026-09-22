#!/usr/bin/env bash
#
# Flash script -- flashes via J-Link using modern nrfutil (the device command).
# More reliable than west's jlink runner (can pair with recover to clear APPROTECT).
#
# Current default target: real board BLEhound (nRF54LM20A), hex taken from build_dongle/ (tools/build.sh's default output).
# All three chips U1/U2/U3 run the same firmware; whichever chip's SWD header (J3/J6/J9) the J-Link is plugged into gets flashed.
# An external J-Link does not power the board, so the board needs USB-C connected separately.
#
# Usage:
#   tools/flash.sh                      # auto-select the hex, flash, and reset
#   tools/flash.sh path/to/app.hex      # specify the hex (e.g. the nRF52840 firmware under build/...)
#   RECOVER=1 tools/flash.sh            # recover first (clear APPROTECT), then flash
#   SN=<jlink serial> tools/flash.sh    # specify the J-Link (by default the sole one is auto-detected)
#
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$PROJ_DIR/tools/jlink_common.sh"

require_nrfutil
SN="$(resolve_jlink_sn)"
echo "== Target: SN=$SN"

# Auto-select the hex: real-board output first (build_dongle/); then the multi-image (with bootloader) merged.hex;
# the single-image sysbuild output is in build/firmware/zephyr/; the non-sysbuild legacy layout is in build/zephyr/.
if [ -n "${1:-}" ]; then
    HEX="$1"
elif [ -f "$PROJ_DIR/build_dongle/firmware/zephyr/zephyr.hex" ]; then
    HEX="$PROJ_DIR/build_dongle/firmware/zephyr/zephyr.hex"
elif [ -f "$PROJ_DIR/build/merged.hex" ]; then
    HEX="$PROJ_DIR/build/merged.hex"
elif [ -f "$PROJ_DIR/build/firmware/zephyr/zephyr.hex" ]; then
    HEX="$PROJ_DIR/build/firmware/zephyr/zephyr.hex"
else
    HEX="$PROJ_DIR/build/zephyr/zephyr.hex"
fi

[ -f "$HEX" ] || { echo "Error: firmware does not exist: $HEX"; exit 1; }

if [ "${RECOVER:-0}" = "1" ]; then
    echo "== recover (clear APPROTECT)"
    "$NRFUTIL_BIN" device recover --serial-number "$SN"
fi

echo "== program $HEX"
"$NRFUTIL_BIN" device program --firmware "$HEX" --serial-number "$SN"
echo "== reset"
"$NRFUTIL_BIN" device reset --serial-number "$SN"
echo "== done"
