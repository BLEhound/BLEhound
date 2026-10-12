#!/usr/bin/env bash
#
# USB DFU -- update the application firmware over the board's own USB port, no J-Link.
#
# The real work is done by tools/blehound_dfu.py next to this script: a self-contained SMP
# (mcumgr) serial client that depends only on pyserial (which extcap already needs). No nrfutil,
# no BLEhound-Analyzer CLI -- the firmware repo does the update on its own, the open-source repo
# just ships the same script.
#
# Flow (firmware side: firmware/blehound/projects/blehound/sysbuild.conf):
#   1. Send HOST_CMD_ENTER_DFU (0x8B) on the app's CDC port; the app writes the boot mode and resets;
#   2. MCUboot starts the firmware loader, USB re-enumerates as "BLEhound Loader" (PID 0x5210);
#   3. SMP uploads the signed app image into image-0, resets back into the app, waits for the app
#      port and reads a status frame to confirm the version.
#   If the board is already in the loader (e.g. a previous update was interrupted), it starts at step 3.
#
# Usage:
#   tools/dfu.sh /dev/cu.usbmodem112101                       # update this board (default: the build's signed image)
#   tools/dfu.sh /dev/cu.usbmodem112101 path/to/blehound_ota.bin
#   tools/dfu.sh COM44 path/to/blehound_ota.bin               # Windows (Git Bash)
#   tools/dfu.sh all [app.bin]                                # update every online BLEhound board in turn
#   tools/dfu.sh ports                                        # list serial ports, tag BLEhound app / loader
#   tools/dfu.sh /dev/cu.usbmodemXXX --list                   # only list the images in the loader (board must be in the loader)
#
# Note: a board's MCUboot only accepts images signed with the key it was flashed with; a rejected
# image leaves the board in the loader -- re-sign with the matching key and upload again.
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PYTHON="${PYTHON:-python3}"
DFU="$PROJ_DIR/tools/blehound_dfu.py"
IMG_DEFAULT="$PROJ_DIR/build/blehound/blehound_ota.bin"

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[ $# -ge 1 ] || usage

case "$1" in
ports)
	exec "$PYTHON" "$DFU" ports
	;;
all)
	IMG="${2:-$IMG_DEFAULT}"
	[ -f "$IMG" ] || { echo "Error: image not found: $IMG"; exit 1; }
	exec "$PYTHON" "$DFU" update all "$IMG"
	;;
esac

PORT="$1"
if [ "${2:-}" = "--list" ]; then
	exec "$PYTHON" "$DFU" list "$PORT"
fi
IMG="${2:-$IMG_DEFAULT}"
[ -f "$IMG" ] || { echo "Error: image not found: $IMG"; exit 1; }
exec "$PYTHON" "$DFU" update "$PORT" "$IMG"
