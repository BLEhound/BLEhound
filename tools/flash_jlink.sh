#!/usr/bin/env bash
#
# Pure SEGGER J-Link erase + flash -- does not depend on nrfutil.
#
# Targets the nRF54LM20A DK (default device=NRF54LM20A_M33), but can be overridden to another chip with DEVICE=.
# This is a separate path from tools/flash.sh (which uses nrfutil): this script only uses a JLinkExe
# commander script to do connect -> erase -> loadfile -> reset.
#
# Usage:
#   tools/flash_jlink.sh                          # auto-select build/firmware/zephyr/zephyr.hex
#   tools/flash_jlink.sh path/to/app.hex          # specify the hex
#   SN=<jlink serial> tools/flash_jlink.sh        # specify the target when there are multiple debuggers
#   DEVICE=nRF52840_xxAA tools/flash_jlink.sh     # switch chip
#   SPEED=4000 tools/flash_jlink.sh               # SWD clock (kHz)
#
# On "unlock / erase a locked chip (APPROTECT)":
#   J-Link Commander's `unlock` command only supports LM3S/Kinetis/EFM32/LPC, **not nRF**.
#   nRF is unlocked automatically by J-Link during `connect` (detecting a locked chip triggers the mass-erase
#   unlock flow). If a factory-locked chip on its first flash does not auto-unlock non-interactively, first run
#   `JLinkExe` interactively once:
#       device NRF54LM20A_M33 -> connect -> (answer y when prompted whether to unsecure) -> erase -> q
#   after that this script works.
#
# The first time you use a newer JLinkExe against a DK's on-board J-Link OB, it may prompt to update the OB firmware --
# run `JLinkExe` interactively once and follow the prompt to finish the update, after which this script can run non-interactively.
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"

JLINK_DIR="${JLINK_DIR:-/opt/SEGGER/JLink}"
JLINKEXE="${JLINKEXE:-$JLINK_DIR/JLinkExe}"
DEVICE="${DEVICE:-NRF54LM20A_M33}"
SPEED="${SPEED:-4000}"

[ -x "$JLINKEXE" ] || { echo "Error: JLinkExe not found: $JLINKEXE"; exit 1; }

# Select the hex: the sysbuild-layout output is in build/firmware/zephyr/; with a bootloader it is merged.hex;
# the non-sysbuild legacy layout is in build/zephyr/.
if [ -n "${1:-}" ]; then
    HEX="$1"
elif [ -f "$PROJ_DIR/build/merged.hex" ]; then
    HEX="$PROJ_DIR/build/merged.hex"
elif [ -f "$PROJ_DIR/build/firmware/zephyr/zephyr.hex" ]; then
    HEX="$PROJ_DIR/build/firmware/zephyr/zephyr.hex"
elif [ -f "$PROJ_DIR/build/zephyr/zephyr.hex" ]; then
    HEX="$PROJ_DIR/build/zephyr/zephyr.hex"
else
    echo "Error: no hex found, build first: tools/build.sh" >&2
    exit 1
fi
[ -f "$HEX" ] || { echo "Error: firmware does not exist: $HEX"; exit 1; }

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
SCRIPT="$WORK_DIR/flash.jlink"

cat > "$SCRIPT" <<EOF
si SWD
speed $SPEED
device $DEVICE
connect
erase
loadfile "$HEX"
r
g
q
EOF

echo "== JLink flash"
echo "   device = $DEVICE"
echo "   hex    = $HEX"
[ -n "${SN:-}" ] && echo "   SN     = $SN"

# macOS ships bash 3.2, where an empty array expands to "unbound" under set -u, so no arrays are used
if [ -n "${SN:-}" ]; then
    "$JLINKEXE" -SelectEmuBySN "$SN" -ExitOnError 1 -CommanderScript "$SCRIPT"
else
    "$JLINKEXE" -ExitOnError 1 -CommanderScript "$SCRIPT"
fi

echo "== done"
