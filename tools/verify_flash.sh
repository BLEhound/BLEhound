#!/usr/bin/env bash
# Byte-by-byte compare the on-chip firmware against the build output zephyr.bin using J-Link, then reset and run (the nRF54L core may stay halted).
# Why this exists: on 2026-09-21 a real device had nrfutil program report success while the board still ran the old firmware; the
# first ~1s of RTT lines after reset are often lost, so you can't judge the version from the boot log. After flashing, always verify with this script.
# Usage: tools/verify_flash.sh <J-Link SN> [zephyr.bin path, defaults to the build_dongle output]
set -uo pipefail
SN="${1:?Usage: tools/verify_flash.sh <J-Link SN> [bin]}"
PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${2:-$PROJ_DIR/build_dongle/firmware/zephyr/zephyr.bin}"
JLINK_DIR="${JLINK_DIR:-/Applications/SEGGER/JLink_V962}"
DEVICE="${DEVICE:-nRF54LM20A_M33}"
[ -f "$BIN" ] || { echo "Error: bin does not exist: $BIN"; exit 1; }
S="$(mktemp)"; printf 'verifybin %s 0x0\nr\ng\nexit\n' "$BIN" > "$S"
"$JLINK_DIR/JLinkExe" -NoGui 1 -device "$DEVICE" -if SWD -speed 4000 -USB "$SN" -AutoConnect 1 -CommandFile "$S" 2>&1 \
	| grep -E "Verify successful|Verify failed|Cannot connect|ERROR" | head -3
