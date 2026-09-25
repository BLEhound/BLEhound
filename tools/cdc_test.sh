#!/usr/bin/env bash
#
# USB CDC ACM presence self-test -- validates that the sniffer's host data channel
# enumerates and its serial port can be opened.
#
# The firmware does NOT echo bytes back: input on CDC is parsed as COBS command
# frames, and the only outbound traffic is captured BLE packets. So this script does
# not do a loopback compare; it checks that the device enumerates with the expected
# USB identity and that its CDC ACM serial port opens in raw mode. That is the useful
# smoke test for "is the data channel alive" before starting a capture in Wireshark.
#
# WARNING prerequisite: the data channel goes over the **nRF52840's own USB peripheral**, not the J-Link debug port.
#    - nRF52840 DK (PCA10056): the board has two micro-USB ports; you must plug an extra cable into
#      the one silkscreened "nRF USB". If you only plug into the debug port (the side with the power
#      switch / SEGGER chip), the host will not see this device at all. Both ports can be plugged in at
#      once without interfering.
#    - nRF52840 Dongle (PCA10059): only one USB port, just plug it in.
#
# Usage:
#   tools/cdc_test.sh                # auto-find the port and check it
#   PORT=/dev/cu.usbmodemXXXX tools/cdc_test.sh
#
set -euo pipefail

# The identity declared by the firmware in prj.conf (CONFIG_USB_DEVICE_VID / _PID / _PRODUCT)
readonly SNIFFER_VID="0x1915"
readonly SNIFFER_PID="0x520F"
readonly SNIFFER_PRODUCT="BLEhound Sniffer"

find_sniffer_port() {
    # macOS: first get the device subtree by USB product name, then pick the serial-port node from that subtree.
    # Read ioreg to the end instead of awk-exiting on the first match: an early exit SIGPIPEs ioreg, and with pipefail + set -e
    # the whole script then dies silently (seen with three boards attached).
    # Note you can't use `ioreg -c IOSerialBSDClient`: that plane has no "USB Product Name",
    # the product name lives on the parent USB node, and the two never appear in the same output.
    if [ "$(uname)" = "Darwin" ]; then
        ioreg -r -n "$SNIFFER_PRODUCT" -l -w 0 2>/dev/null \
            | awk '
                /"IOCalloutDevice"/ && !done {
                    match($0, /"\/dev\/[^"]+"/)
                    if (RSTART) { print substr($0, RSTART + 1, RLENGTH - 2); done = 1 }
                }'
        return
    fi

    # Linux: match /dev/serial/by-id by VID/PID
    local vid pid
    vid="$(printf '%04x' "$SNIFFER_VID")"
    pid="$(printf '%04x' "$SNIFFER_PID")"
    for dev in /dev/ttyACM*; do
        [ -e "$dev" ] || continue
        if udevadm info -q property -n "$dev" 2>/dev/null \
            | grep -qi "ID_VENDOR_ID=$vid"; then
            echo "$dev"
            return
        fi
    done
}

PORT="${PORT:-$(find_sniffer_port)}"

if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
    echo "== FAIL: could not find the sniffer's CDC ACM serial port ($SNIFFER_PRODUCT, $SNIFFER_VID:$SNIFFER_PID)"
    echo
    echo "  On the DK (PCA10056), make sure you have plugged an extra micro-USB cable into the port silkscreened 'nRF USB' --"
    echo "  when only the debug port (J-Link) is plugged in, this device does not enumerate on the host."
    echo
    echo "Current serial devices:"
    ls /dev/cu.* /dev/ttyACM* 2>/dev/null || true
    exit 1
fi

echo "== Port: $PORT"

# CDC ACM does not care about the baud rate, but the terminal still needs to be set to raw, otherwise local echo / line discipline will corrupt the data
if [ "$(uname)" = "Darwin" ]; then
    stty -f "$PORT" raw -echo 115200
else
    stty -F "$PORT" raw -echo 115200
fi

# Open the port read/write on fd 3 -- if this succeeds the CDC ACM channel is alive.
if exec 3<>"$PORT"; then
    exec 3<&-
    exec 3>&-
    echo "== PASS: sniffer enumerated and its CDC ACM port opened"
    echo "   (start a capture in Wireshark to see packets; the firmware does not echo test bytes)"
    exit 0
fi

echo "== FAIL: found $PORT but could not open it (busy or permission denied)"
exit 1
