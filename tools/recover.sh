#!/usr/bin/env bash
#
# Unlock script -- runs nrfutil device recover on the target, erasing the whole chip and clearing APPROTECT.
# Use this when the chip is locked by readback protection (flashing/debugging reports protected).
#
# WARNING this erases all flash content on the chip, including the bootloader and any stored config.
#
# Usage:
#   tools/recover.sh              # auto-detect the sole J-Link target
#   SN=<jlink serial> tools/recover.sh
#
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$PROJ_DIR/tools/jlink_common.sh"

require_nrfutil
SN="$(resolve_jlink_sn)"

echo "== recover (full-chip erase + clear APPROTECT) SN=$SN"
"$NRFUTIL_BIN" device recover --serial-number "$SN"
echo "== done"
