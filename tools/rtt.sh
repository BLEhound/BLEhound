#!/usr/bin/env bash
#
# View RTT logs -- capture the firmware's SEGGER RTT output via J-Link.
#
# Implementation notes:
#   Don't use JLinkRTTLogger -- in practice under V8.76 it fails to find the RTT control block ("RTT Control Block
#   not found") even when the control block magic in the firmware is intact. Use JLinkExe instead: once it connects to
#   the target it opens an RTT telnet service on local port 19021, which you can read with nc -- stable and without
#   needing to know the control block address.
#
# Usage:
#   tools/rtt.sh            # print RTT continuously (Ctrl-C to quit)
#   tools/rtt.sh 15         # capture for 15 seconds then auto-quit (for scripted self-tests)
#   SN=<jlink serial> tools/rtt.sh   # specify the target when there are multiple debuggers
#   RESET=1 tools/rtt.sh    # reset first then capture, so you can see the boot logs (the fem_ctrl/board_role lines)
#
# Current default target: real board BLEhound (nRF54LM20A, JLink V9.62, device name nRF54LM20A_M33).
# For the nRF52840 DK: JLINK_DIR=/opt/SEGGER/JLink DEVICE=nRF52840_xxAA tools/rtt.sh
#
set -uo pipefail

# JLink V8.76 does not recognize nRF54LM20A; the real board requires V9.62+ (device name nRF54LM20A_M33).
JLINK_DIR="${JLINK_DIR:-/Applications/SEGGER/JLink_V962}"
JLINKEXE="${JLINKEXE:-$JLINK_DIR/JLinkExe}"
DEVICE="${DEVICE:-nRF54LM20A_M33}"          # for the nRF52840 DK pass DEVICE=nRF52840_xxAA
RTT_PORT="${RTT_PORT:-19021}"
# The RTT control block is in RAM, so a search range covering all of RAM is enough (both nRF54LM20A/nRF52840 are 256KB starting at 0x20000000).
RTT_SEARCH_RANGE="${RTT_SEARCH_RANGE:-0x20000000 0x40000}"   # covers all 256KB of nRF54LM20A RAM
DUR="${1:-0}"

# JLinkExe's sleep unit is milliseconds; DUR=0 means "run forever" (give it a large enough value and rely on Ctrl-C to quit)
if [ "$DUR" -gt 0 ]; then
    SLEEP_MS=$(( (DUR + 3) * 1000 ))
else
    SLEEP_MS=86400000   # 24h
fi

[ -x "$JLINKEXE" ] || { echo "Error: JLinkExe not found: $JLINKEXE"; exit 1; }

WORK_DIR="$(mktemp -d)"
SCRIPT="$WORK_DIR/rtt.jlink"

# Two actions that must be done explicitly:
#  1) SetRTTSearchRanges -- J-Link's default search range does not find the control block Zephyr places
#     around 0x20000410, and without this line you get "connected but not a single byte comes out".
#  2) g (resume) -- if a previous debug session halted the core (JLinkExe's h / exit),
#     the core stays stopped, so naturally there is no new RTT data. resume has no side effect on a target that is already running.
#
# When RESET=1, the reset must **come after the reader has attached**: the RTT buffer is only 1KB and drops new data
# when full, so resetting before attaching means the first few seconds of boot logs are already squeezed out. Here we let
# JLinkExe sleep long enough for ATTACH_DELAY, giving the nc below time to connect, then send r -- so we can capture from the very first boot line.
ATTACH_DELAY="${RTT_ATTACH_DELAY:-4}"

if [ "${RESET:-0}" = "1" ]; then
    RUN_CMDS="sleep $(( (ATTACH_DELAY + 2) * 1000 ))"$'\nr\ng'
else
    RUN_CMDS='g'
fi

cat > "$SCRIPT" <<EOF
si SWD
speed 4000
device $DEVICE
connect
exec SetRTTSearchRanges = $RTT_SEARCH_RANGE
$RUN_CMDS
sleep $SLEEP_MS
q
EOF

# Note: macOS ships bash 3.2, where an empty array expands to "unbound" under set -u, so no arrays are used
if [ -n "${SN:-}" ]; then
    "$JLINKEXE" -RTTTelnetPort "$RTT_PORT" -SelectEmuBySN "$SN" -CommanderScript "$SCRIPT" > "$WORK_DIR/jlink.log" 2>&1 &
else
    "$JLINKEXE" -RTTTelnetPort "$RTT_PORT" -CommanderScript "$SCRIPT" > "$WORK_DIR/jlink.log" 2>&1 &
fi
JLINK_PID=$!

cleanup() {
    kill "$JLINK_PID" 2>/dev/null
    if [ "${RTT_KEEP:-0}" = "1" ]; then
        echo "== debug: keeping J-Link work directory $WORK_DIR" >&2
    else
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT INT TERM

# Wait for J-Link to connect to the target.
# Never use `nc -z` to probe 19021: that opens a momentary TCP connection, J-Link pushes all its backlogged RTT
# data to it and then loses it when the connection closes, so the real reader receives nothing.
# Instead wait for JLinkExe's own log to show the "core identified" marker.
READY=0
for _ in $(seq 1 40); do
    if grep -qE "identified|Cortex-M" "$WORK_DIR/jlink.log" 2>/dev/null; then
        READY=1
        break
    fi
    if ! kill -0 "$JLINK_PID" 2>/dev/null; then
        break
    fi
    sleep 0.5
done

if [ "$READY" != "1" ]; then
    echo "Error: J-Link failed to connect to the target, JLinkExe output:"
    tail -20 "$WORK_DIR/jlink.log"
    exit 1
fi

# Connected to the target != RTT control block found -- J-Link only scans RAM after connecting,
# so attaching too early gets an empty session with no buffer bound yet. Leave time for the scan.
sleep "$ATTACH_DELAY"

echo "== RTT connected (device=$DEVICE, port=$RTT_PORT), Ctrl-C to quit"

# `-d` = don't read from stdin: a background nc's stdin is /dev/null, which EOFs on the first read, so it
# half-closes the socket and sends FIN, and J-Link considers the client disconnected and stops pushing -- showing up as "connected but no data".
if [ "$DUR" -gt 0 ]; then
    nc -d localhost "$RTT_PORT" &
    NC_PID=$!
    sleep "$DUR"
    kill "$NC_PID" 2>/dev/null
    wait "$NC_PID" 2>/dev/null
    exit 0          # a timed capture finished normally, don't treat kill's return code as a failure
else
    nc -d localhost "$RTT_PORT"
fi
