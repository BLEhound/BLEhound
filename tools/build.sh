#!/usr/bin/env bash
#
# Local build script.
#
# Both workspace layouts are supported:
#   1) Official: the directory containing BLEhound is itself a west workspace (NCS already pulled via west update)
#      -- it is used automatically.
#   2) Quick: reuse an existing NCS workspace without re-downloading -- set NCS_TOPDIR to point at it.
#
# Usage:
#   tools/build.sh                          # default: real board BLEhound (3x nRF54LM20A + nRF21540)
#                                           #   = DK target nrf54lm20dk/nrf54lm20a/cpuapp
#                                           #     + boards/blehound.{conf,overlay} (LFRC / real-board balls)
#                                           #   output build_dongle/firmware/zephyr/zephyr.hex
#   tools/build.sh -- --pristine            # real board, full rebuild
#   V2=1 tools/build.sh                     # revised board (V2): also overlay boards/blehound_v2.overlay
#                                           #   (inter-chip SPIM00 moved to the datasheet's dedicated pins P2.01/P2.02/P2.04), output build_dongle_v2/
#                                           #   WARNING the first-batch flywire board cannot flash the V2 output (P2.01 is its old SYNC net)
#   BLEHOUND=0 tools/build.sh nrf52840dk/nrf52840  # legacy path: nRF52840 DK/Dongle (output build/)
#   NCS_TOPDIR=/path/to/ncs tools/build.sh  # specify the NCS workspace
#
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"        # = repo root (BLEhound)
WS_TOP="$(cd "$PROJ_DIR/.." && pwd)"                # workspace top level (parent dir of BLEhound)

# Choose the NCS workspace (west topdir):
if [ -n "${NCS_TOPDIR:-}" ]; then
    :
elif [ -f "$WS_TOP/.west/config" ]; then
    NCS_TOPDIR="$WS_TOP"                            # official: this workspace has already been west update'd
else
    echo "Error: no west workspace found. First run 'west init -l BLEhound && west update' above the repo, or specify one with NCS_TOPDIR=/path/to/ncs." >&2; exit 1
fi

# Zephyr SDK: the official flow uses 1.0.1 (requires NCS v3.4.0); can be overridden with ZEPHYR_SDK_INSTALL_DIR.
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-1.0.1}"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

# BLEHOUND=1 (default): real-board overlay layer. The board is fixed to the nrf54lm20dk target and the output goes to its own build_dongle/,
# to avoid cross-contaminating the cmake cache with a pure DK build (build/).
BLEHOUND="${BLEHOUND:-1}"
if [ "$BLEHOUND" = "1" ] && [ "${V2:-0}" = "1" ]; then
    BOARD="${1:-nrf54lm20dk/nrf54lm20a/cpuapp}"
    BUILD_DIR="${BUILD_DIR:-$PROJ_DIR/build_dongle_v2}"
    BLEHOUND_ARGS="-DEXTRA_CONF_FILE=boards/blehound.conf -DEXTRA_DTC_OVERLAY_FILE=boards/blehound.overlay;boards/blehound_v2.overlay"
elif [ "$BLEHOUND" = "1" ]; then
    BOARD="${1:-nrf54lm20dk/nrf54lm20a/cpuapp}"
    BUILD_DIR="${BUILD_DIR:-$PROJ_DIR/build_dongle}"
    BLEHOUND_ARGS="-DEXTRA_CONF_FILE=boards/blehound.conf -DEXTRA_DTC_OVERLAY_FILE=boards/blehound.overlay"
else
    BOARD="${1:-nrf52840dk/nrf52840}"
    BUILD_DIR="${BUILD_DIR:-$PROJ_DIR/build}"
    BLEHOUND_ARGS=""
fi
shift || true
# Allow the "tools/build.sh -- --pristine" form that passes only west args (when the first arg is --, do not treat it as the board name).
[ "${BOARD}" = "--" ] && { BOARD="$( [ "$BLEHOUND" = "1" ] && echo nrf54lm20dk/nrf54lm20a/cpuapp || echo nrf52840dk/nrf52840 )"; }

echo "== NCS_TOPDIR : $NCS_TOPDIR"
echo "== SDK        : $ZEPHYR_SDK_INSTALL_DIR"
echo "== BOARD      : $BOARD"
echo "== APP        : $PROJ_DIR/firmware"
echo "== BUILD_DIR  : $BUILD_DIR"

case "$PROJ_DIR" in *" "*)
    echo "⚠️  Path contains spaces; the Zephyr build may fail. Use a space-free directory or a symlink." ;;
esac

cd "$NCS_TOPDIR"
# Only what follows west's -- are cmake args; merge the real-board overlay args with the caller's args into one group
# (under bash 3.2 + set -u an empty array expands to "unbound", so use set -- to rebuild the positional params).
WEST_PART=""; CMAKE_PART="$BLEHOUND_ARGS"
seen_dd=0
for a in "$@"; do
    if [ "$a" = "--" ]; then seen_dd=1; continue; fi
    if [ $seen_dd = 1 ]; then CMAKE_PART="$CMAKE_PART $a"; else WEST_PART="$WEST_PART $a"; fi
done
# shellcheck disable=SC2086  # the args contain no spaces, so splitting on whitespace is intentional
if [ -n "${CMAKE_PART// /}" ]; then
    set -- $WEST_PART -- $CMAKE_PART
else
    set -- $WEST_PART
fi
exec west build -b "$BOARD" -d "$BUILD_DIR" -s "$PROJ_DIR/firmware" "$@"
