#!/usr/bin/env bash
#
# Flash script -- flashes via J-Link using modern nrfutil (the device command).
# More reliable than west's jlink runner (can pair with recover to clear APPROTECT).
#
# Current default target: real board BLEhound (nRF54LM20A), hex taken from build/ (tools/build.sh's default output).
# All three chips U1/U2/U3 run the same firmware; whichever chip's SWD header (J3/J6/J9) the J-Link is plugged into gets flashed.
# An external J-Link does not power the board, so the board needs USB-C connected separately.
#
# Usage:
#   tools/flash.sh                      # auto-select the hex, flash, and reset
#   tools/flash.sh path/to/app.hex      # specify the hex explicitly
#   RECOVER=1 tools/flash.sh            # recover first (clear APPROTECT), then flash
#   SN=<jlink serial> tools/flash.sh    # specify the J-Link (by default the sole one is auto-detected)
#
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$PROJ_DIR/tools/jlink_common.sh"

require_nrfutil
SN="$(resolve_jlink_sn)"
echo "== Target: SN=$SN"

# Auto-select the hex: real-board output first (build/); then the multi-image (with bootloader) merged.hex;
# the sysbuild whole-chip output is in build/blehound/blehound_merged.hex.
# With MCUboot the merged_*.hex (MCUboot + firmware loader + signed app) must be flashed: the app's own zephyr.hex
# is linked at 0x23000 and cannot run without the bootloader. Erase the whole chip first: the app moved from 0x0 to
# 0x23000, MCUboot would treat old-firmware leftovers as garbage, but a clean chip is safer.
if [ -n "${1:-}" ]; then
    HEX="$1"
elif [ -f "$PROJ_DIR/build/blehound/blehound_merged.hex" ]; then
    HEX="$PROJ_DIR/build/blehound/blehound_merged.hex"
elif [ -f "$PROJ_DIR/build_v2/blehound/blehound_merged.hex" ]; then
    HEX="$PROJ_DIR/build_v2/blehound/blehound_merged.hex"
else
    echo "Error: no hex found, build first: tools/build.sh" >&2
    exit 1
fi

[ -f "$HEX" ] || { echo "Error: firmware does not exist: $HEX"; exit 1; }

if [ "${RECOVER:-0}" = "1" ]; then
    echo "== recover (clear APPROTECT)"
    "$NRFUTIL_BIN" device recover --serial-number "$SN"
fi

echo "== program $HEX"
"$NRFUTIL_BIN" device program --firmware "$HEX" --serial-number "$SN" --options chip_erase_mode=ERASE_ALL
echo "== reset"
"$NRFUTIL_BIN" device reset --serial-number "$SN"
echo "== done"
