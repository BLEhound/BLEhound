#!/bin/bash
# First-time setup of the self-contained BLEhound west workspace:
#   workspace root = <repo>/firmware/ ; manifest repo = firmware/blehound/ (west.yml) ;
#   `west update` pulls NCS v3.4.0 (nrf/zephyr/nrfxlib/mcuboot/mbedtls/oberon) under firmware/.
#
# Run this BEFORE opening VSCode — VSCode's Git/indexing extensions can lock freshly
# cloned files and make `west init` fail. Keep the path free of spaces.
set -e

command -v west >/dev/null 2>&1 || { echo "Error: west not found. Set up your Zephyr environment first (west on PATH)."; exit 1; }

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SELF_DIR/.." && pwd)"
WS="$REPO_ROOT/firmware"

[ -f "$WS/blehound/west.yml" ] || { echo "Error: $WS/blehound/west.yml not found"; exit 1; }
case "$WS" in *" "*) echo "Warning: workspace path contains spaces ($WS); Zephyr/west/cmake dislike them." ;; esac

cd "$WS"
unset ZEPHYR_BASE   # let west locate zephyr from the workspace

if [ -f "$WS/.west/config" ]; then
    echo "==> Workspace already initialized ($WS), skipping west init"
else
    [ -d "$WS/.west" ] && { echo "==> Cleaning incomplete .west ..."; rm -rf "$WS/.west"; }
    west init -l blehound
fi

west update
west zephyr-export
echo "==> Done. Build with tools/build.sh or the VSCode tasks."
