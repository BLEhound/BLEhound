#!/usr/bin/env bash
#
# J-Link / nrfutil common logic -- to be sourced by flash.sh, recover.sh, etc., not run standalone.
#
# Provides:
#   NRFUTIL_BIN          path to the modern nrfutil executable
#   resolve_jlink_sn     resolve the target board's serial number (the SN env var takes priority, otherwise auto-detect the sole J-Link)

NRFUTIL_BIN="${NRFUTIL:-$HOME/.nrfutil/bin/nrfutil}"

# Usage: SN="$(resolve_jlink_sn)" -- on failure it prints the reason and returns non-zero
resolve_jlink_sn() {
    if [ -n "${SN:-}" ]; then
        printf '%s' "$SN"
        return 0
    fi

    # nrfutil --json output is JSONL (one event per line); take the one carrying devices;
    # only accept the jlink trait -- only an on-board debugger can flash, a DFU dongle does not go this route.
    local candidates count
    candidates="$("$NRFUTIL_BIN" device list --json 2>/dev/null \
        | python3 -c '
import json, sys

seen = []
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        evt = json.loads(line)
    except ValueError:
        continue
    for dev in (evt.get("data") or {}).get("devices", []):
        sn = dev.get("serialNumber")
        if (dev.get("traits") or {}).get("jlink") and sn not in seen:
            seen.append(sn)
for sn in seen:
    print(sn)
' 2>/dev/null || true)"

    count="$(printf '%s' "$candidates" | grep -c . || true)"

    if [ "$count" = "1" ]; then
        printf '%s' "$candidates"
        return 0
    fi

    if [ "$count" = "0" ]; then
        echo "Error: no target board with a J-Link detected, check the USB connection or specify one with SN=<serial>" >&2
        "$NRFUTIL_BIN" device list >&2 || true
    else
        echo "Error: multiple J-Link targets detected, specify one of them with SN=<serial>:" >&2
        printf '%s\n' "$candidates" >&2
    fi
    return 1
}

require_nrfutil() {
    [ -x "$NRFUTIL_BIN" ] || {
        echo "Error: modern nrfutil not found: $NRFUTIL_BIN" >&2
        return 1
    }
}
