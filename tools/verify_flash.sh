#!/usr/bin/env bash
# Byte-by-byte compare the on-chip firmware against the build outputs using J-Link, then reset and run (the nRF54L core may stay halted).
# Why this exists: on 2026-09-21 a real device had nrfutil program report success while the board still ran the old firmware; the
# first ~1s of RTT lines after reset are often lost, so you can't judge the version from the boot log. After flashing, always verify with this script.
#
# Three-image layout with MCUboot (firmware/boards/common/blehound_partitions.dtsi), compared region by region:
#   0x00000 MCUboot              build/blehound/blehound_mcu_boot.bin
#   0x06000 firmware loader      build/blehound/blehound_loader.bin
#   0x23000 app (signed image)   build/blehound/blehound_app.bin
# Usage: tools/verify_flash.sh <J-Link SN> [build dir, defaults to build]
#        legacy single-image layout: tools/verify_flash.sh <SN> path/to/zephyr.bin (a .bin file = compare 0x0 only)
set -uo pipefail
SN="${1:?Usage: tools/verify_flash.sh <J-Link SN> [build dir|zephyr.bin]}"
PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${2:-$PROJ_DIR/build}"
JLINK_DIR="${JLINK_DIR:-/Applications/SEGGER/JLink_V962}"
DEVICE="${DEVICE:-nRF54LM20A_M33}"

S="$(mktemp)"
if [ -f "$TARGET" ]; then
	printf 'verifybin %s 0x0\n' "$TARGET" > "$S"
elif [ -f "$TARGET/blehound/blehound_mcu_boot.bin" ]; then
	for pair in "blehound/blehound_mcu_boot.bin 0x0" \
	            "blehound/blehound_loader.bin 0x6000" \
	            "blehound/blehound_app.bin 0x23000"; do
		set -- $pair
		[ -f "$TARGET/$1" ] || { echo "Error: build output missing: $TARGET/$1"; exit 1; }
		printf 'verifybin %s %s\n' "$TARGET/$1" "$2" >> "$S"
	done
else
	echo "Error: no build output found: $TARGET"; exit 1
fi
printf 'r\ng\nexit\n' >> "$S"
"$JLINK_DIR/JLinkExe" -NoGui 1 -device "$DEVICE" -if SWD -speed 4000 -USB "$SN" -AutoConnect 1 -CommandFile "$S" 2>&1 \
	| grep -E "Verify successful|Verify failed|Cannot connect|ERROR" | head -4
