#!/usr/bin/env bash
#
# Local build script.
#
# Self-contained west workspace: the workspace root is <repo>/firmware/ (run
# .vscode/blehound_west_init.sh once to pull NCS). Override with NCS_TOPDIR to reuse another workspace.
#
# Usage:
#   tools/build.sh                          # default: first-batch board V1 = board target blehound_v1/nrf54lm20a/cpuapp
#                                           #   (board definition in firmware/blehound/boards/blehound_v1/, auto-discovered via module.yml)
#                                           #   outputs (sysbuild, three images; DFU design in firmware/blehound/projects/blehound/sysbuild.conf) collected in build/blehound/:
#                                           #     blehound_mcu_boot.{bin,hex,elf,map}  MCUboot
#                                           #     blehound_loader.{bin,hex,elf,map}    firmware loader
#                                           #     blehound_app.{bin,hex,elf,map}       application (signed image)
#                                           #     blehound_ota.bin / blehound_ota.zip  app image for DFU upload / DFU package
#                                           #     blehound_merged.hex                  whole-chip image (MCUboot+loader+app)
#   tools/build.sh -- --pristine            # full rebuild
#   V2=1 tools/build.sh                     # revised board V2 = board target blehound_v2/nrf54lm20a/cpuapp, output build_v2/
#                                           #   (inter-chip SPIM00 on the datasheet's dedicated pins P2.01/P2.02/P2.04, FEM SPI moved to P1)
#                                           #   WARNING the first-batch flywire board cannot flash the V2 output (P2.01 is its old SYNC net)
#   NCS_TOPDIR=/path/to/ncs tools/build.sh  # specify the NCS workspace
#
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"        # = repo root (BLEhound)
APP="$PROJ_DIR/firmware/blehound/projects/blehound" # application dir

# west workspace (topdir). Self-contained layout: the root is <repo>/firmware/ (run
# .vscode/blehound_west_init.sh once). Override with NCS_TOPDIR to reuse another workspace.
NCS_TOPDIR="${NCS_TOPDIR:-$PROJ_DIR/firmware}"
if [ ! -f "$NCS_TOPDIR/.west/config" ]; then
    echo "Error: no west workspace at $NCS_TOPDIR. First run 'cd firmware && west init -l blehound && west update', or set NCS_TOPDIR=/path/to/ncs." >&2; exit 1
fi

# Zephyr SDK: the official flow uses 1.0.1 (requires NCS v3.4.0); can be overridden with ZEPHYR_SDK_INSTALL_DIR.
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-1.0.1}"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

# One board target per PCB revision (firmware/blehound/boards/blehound_v1, blehound_v2). Board defs are
# auto-discovered via blehound/zephyr/module.yml (board_root), so no -DBOARD_ROOT is needed.
if [ "${V2:-0}" = "1" ]; then
    BOARD="${1:-blehound_v2/nrf54lm20a/cpuapp}"
    BUILD_DIR="${BUILD_DIR:-$PROJ_DIR/build_v2}"
else
    BOARD="${1:-blehound_v1/nrf54lm20a/cpuapp}"
    BUILD_DIR="${BUILD_DIR:-$PROJ_DIR/build}"
fi
BLEHOUND_ARGS=""

# MCUboot signing key: BLEHOUND_SIGNING_KEY (a PEM) if set, else ~/.blehound/keys/blehound_ed25519.pem
# (made by tools/keygen.sh, **never committed**; the public key is kept in firmware/keys/blehound_ed25519.pub.pem).
# With neither present the build falls back to MCUboot's bundled development key and warns: such an MCUboot only
# accepts images signed with the dev key and must not be released. After changing the key the public key inside
# MCUboot changes too, so existing boards need one J-Link programming of the merged hex before DFU accepts new images.
SIGNING_KEY="${BLEHOUND_SIGNING_KEY:-$HOME/.blehound/keys/blehound_ed25519.pem}"
if [ -f "$SIGNING_KEY" ]; then
    BLEHOUND_ARGS="$BLEHOUND_ARGS -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$SIGNING_KEY\""
    echo "== SIGN KEY   : $SIGNING_KEY"
else
    echo "WARNING: no signing key at $SIGNING_KEY, using MCUboot's development key (local debugging only, do not release). Create one with tools/keygen.sh"
fi
shift || true
# Allow the "tools/build.sh -- --pristine" form that passes only west args (when the first arg is --, do not treat it as the board name).
# To pass cmake args (-D...) add another -- after the west args: tools/build.sh -- --pristine -- -DCONFIG_X=y
[ "${BOARD}" = "--" ] && BOARD="$( [ "${V2:-0}" = "1" ] && echo blehound_v2/nrf54lm20a/cpuapp || echo blehound_v1/nrf54lm20a/cpuapp )"

echo "== WS         : $NCS_TOPDIR"
echo "== SDK        : $ZEPHYR_SDK_INSTALL_DIR"
echo "== BOARD      : $BOARD"
echo "== APP        : $APP"
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
exec west build -b "$BOARD" -d "$BUILD_DIR" -s "$APP" "$@"
