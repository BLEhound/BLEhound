#!/usr/bin/env bash
#
# USB DFU -- update the application firmware over the board's own USB port, no J-Link needed.
#
# Flow (firmware-side design in firmware/sysbuild.conf):
#   1. send HOST_CMD_ENTER_DFU (0x8B) to the app's CDC serial port; the app writes the boot mode and resets;
#   2. MCUboot starts the firmware loader, USB re-enumerates as "BLEhound Loader" (PID 0x5210), the port name usually stays;
#   3. nrfutil mcu-manager uploads the signed app image into image-0 over SMP, then resets back into the app.
#   If the board is already in the loader (e.g. a previous update was interrupted), start from step 3.
#
# Usage:
#   tools/dfu.sh /dev/cu.usbmodem112101                       # update this board (default: the signed image from build_dongle)
#   tools/dfu.sh /dev/cu.usbmodem112101 path/to/blehound_ota.bin
#   tools/dfu.sh all                                          # update every "BLEhound Sniffer" port in turn (macOS)
#   tools/dfu.sh /dev/cu.usbmodemXXX --list                   # only list the images the loader reports
#
# Requires: nrfutil's mcu-manager command (`nrfutil install mcu-manager`), python3 + pyserial.
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NRFUTIL_BIN="${NRFUTIL:-$HOME/.nrfutil/bin/nrfutil}"
PYTHON="${PYTHON:-python3}"
IMG_DEFAULT="$PROJ_DIR/build_dongle/blehound/blehound_ota.bin"

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[ $# -ge 1 ] || usage
"$NRFUTIL_BIN" mcu-manager --help >/dev/null 2>&1 || { echo "Error: nrfutil lacks the mcu-manager command, run first: $NRFUTIL_BIN install mcu-manager"; exit 1; }

# macOS: tell from the USB product name whether a port is currently the app or the loader (on Linux /dev/ttyACM* has no such info, the caller passes the right port).
usb_names() { ioreg -p IOUSB -l -w0 2>/dev/null | grep -E '"USB Product Name" = "BLEhound' | sed 's/.*= "//; s/"//' | sort | uniq -c; }
count_loader() { usb_names | awk '/BLEhound Loader/ {print $1}' ; }

enter_dfu() {   # $1 = serial port
	"$PYTHON" - "$1" <<'PY'
import sys, time, serial
port = sys.argv[1]
def cobs(data):
    out = bytearray(); idx = 0; code = 1; out.append(0)
    for b in data:
        if b == 0:
            out[idx] = code; idx = len(out); out.append(0); code = 1
        else:
            out.append(b); code += 1
            if code == 0xFF:
                out[idx] = code; idx = len(out); out.append(0); code = 1
    out[idx] = code
    return bytes(out)
with serial.Serial(port, 115200, timeout=0.2) as s:
    s.dtr = True; time.sleep(0.1)
    s.write(cobs(bytes([0x8B])) + b"\x00"); s.flush(); time.sleep(0.2)
PY
}

wait_loader() {   # wait until the number of "BLEhound Loader" devices becomes $1, at most 10 s
	for _ in $(seq 1 20); do
		[ "$(count_loader)" = "$1" ] && return 0
		sleep 0.5
	done
	return 1
}

dfu_one() {   # $1 = serial port, $2 = image
	local port="$1" img="$2"
	echo "== $port: entering DFU mode"
	# Ask over SMP first: a board already in the loader (interrupted update / rejected image) must not get capture commands.
	if "$NRFUTIL_BIN" mcu-manager serial image-list --serial-port "$port" --timeout 2 >/dev/null 2>&1; then
		echo "   already in the loader, skipping the enter step"
	else
		local before; before="$(count_loader)"; before="${before:-0}"
		enter_dfu "$port" || { echo "   serial port cannot be opened, cannot send the enter command"; return 1; }
		wait_loader $((before + 1)) || echo "   (no new BLEhound Loader enumerated, trying anyway)"
		sleep 1
	fi
	echo "== $port: images reported by the loader"
	"$NRFUTIL_BIN" mcu-manager serial image-list --serial-port "$port" --timeout 10 | grep -E "slot|version" | paste - - | sed 's/^/   /'
	echo "== $port: uploading $img"
	"$NRFUTIL_BIN" mcu-manager serial image-upload --serial-port "$port" --firmware "$img" --timeout 60
	echo "== $port: resetting into the app"
	"$NRFUTIL_BIN" mcu-manager serial reset --serial-port "$port" --timeout 10
	echo "== $port: done"
}

if [ "$1" = "all" ]; then
	IMG="${2:-$IMG_DEFAULT}"
	[ -f "$IMG" ] || { echo "Error: image not found: $IMG"; exit 1; }
	PORTS="$(ls /dev/cu.usbmodem* 2>/dev/null | grep -v usbmodem0006 || true)"
	[ -n "$PORTS" ] || { echo "Error: no /dev/cu.usbmodem* serial ports found"; exit 1; }
	for p in $PORTS; do dfu_one "$p" "$IMG"; sleep 2; done
	exit 0
fi

PORT="$1"
if [ "${2:-}" = "--list" ]; then
	"$NRFUTIL_BIN" mcu-manager serial image-list --serial-port "$PORT" --timeout 10
	exit 0
fi
IMG="${2:-$IMG_DEFAULT}"
[ -f "$IMG" ] || { echo "Error: image not found: $IMG"; exit 1; }
dfu_one "$PORT" "$IMG"
