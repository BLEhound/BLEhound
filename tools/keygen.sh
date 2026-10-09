#!/usr/bin/env bash
# Generate the MCUboot signing key (ed25519): the private key goes outside the repo to ~/.blehound/keys/blehound_ed25519.pem
# (0600, **never commit it**), the public key is exported to firmware/keys/blehound_ed25519.pub.pem for reference.
# tools/build.sh picks the private key up automatically.
# Usage: tools/keygen.sh            # keeps an existing key
#        FORCE=1 tools/keygen.sh    # regenerate (existing boards then need a J-Link reflash of MCUboot to accept the new key)
set -euo pipefail
PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KEY="${BLEHOUND_SIGNING_KEY:-$HOME/.blehound/keys/blehound_ed25519.pem}"
PUB="$PROJ_DIR/firmware/blehound/projects/blehound/keys/blehound_ed25519.pub.pem"
NCS_TOPDIR="${NCS_TOPDIR:-$PROJ_DIR/firmware}"
IMGTOOL="$NCS_TOPDIR/bootloader/mcuboot/scripts/imgtool.py"
PYTHON="${PYTHON:-$HOME/ENV_TOOL/Python312/bin/python3}"
[ -f "$IMGTOOL" ] || { echo "imgtool not found: $IMGTOOL (set NCS_TOPDIR)"; exit 1; }
mkdir -p "$(dirname "$KEY")"
if [ -f "$KEY" ] && [ "${FORCE:-0}" != "1" ]; then
    echo "== key exists, keeping: $KEY"
else
    "$PYTHON" "$IMGTOOL" keygen -k "$KEY" -t ed25519
    chmod 600 "$KEY"
    echo "== generated: $KEY"
fi
"$PYTHON" "$IMGTOOL" getpub -k "$KEY" -e pem > "$PUB"
echo "== public key written: $PUB"
