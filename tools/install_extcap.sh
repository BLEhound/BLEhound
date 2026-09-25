#!/usr/bin/env bash
#
# One-command install of the extcap plugin into Wireshark.
#
# After installing, restart Wireshark and "BLEhound Sniffer" will appear in the interface list.
#
# Usage:
#   tools/install_extcap.sh            # install
#   tools/install_extcap.sh --uninstall
#
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PLUGIN="$PROJ_DIR/host/blehound_extcap.py"
PLUGIN_NAME="blehound_extcap.py"

#
# Determine the extcap directory.
#
# WARNING don't guess the path from old tutorials online -- Wireshark 4.x moved the personal extcap
#    directory from ~/.config/wireshark/extcap to ~/.local/lib/wireshark/extcap, and installing to the
#    wrong place shows up as "installed but not in the interface list", which is hard to debug.
#    The most reliable approach is to ask Wireshark itself: tshark -G folders.
#
resolve_extcap_dir() {
	if command -v tshark >/dev/null 2>&1; then
		local dir
		dir="$(tshark -G folders 2>/dev/null \
			| awk -F'\t' '/^Personal Extcap path:/ {print $2; exit}' \
			| sed 's/[[:space:]]*$//')"
		if [ -n "$dir" ]; then
			echo "$dir"
			return 0
		fi
	fi

	# Fallback when tshark is not installed (GUI only): try versions newest to oldest
	case "$(uname)" in
	Darwin | Linux)
		echo "$HOME/.local/lib/wireshark/extcap"
		;;
	*)
		return 1
		;;
	esac
}

case "$(uname)" in
Darwin | Linux) ;;
*)
	echo "Error: this script only supports macOS / Linux."
	echo "      On Windows, copy host/blehound_extcap.py, host/blehound_extcap.bat and"
	echo "      host/blehound_tri_aggregator.py into Wireshark's extcap directory;"
	echo "      find the exact path in Wireshark: Help -> About Wireshark -> Folders -> Personal Extcap path"
	exit 1
	;;
esac

EXTCAP_DIR="$(resolve_extcap_dir)"

if [ "${1:-}" = "--uninstall" ]; then
	removed=0
	# Also clean up the old paths of historical versions, so no stale copy is left behind
	for dir in "$EXTCAP_DIR" "$HOME/.config/wireshark/extcap" "$HOME/.wireshark/extcap"; do
		if [ -e "$dir/$PLUGIN_NAME" ]; then
			rm -f "$dir/$PLUGIN_NAME"
			echo "Removed $dir/$PLUGIN_NAME"
			removed=1
		fi
	done
	[ "$removed" = "1" ] || echo "No installed plugin found"
	echo "== Done, restart Wireshark to take effect"
	exit 0
fi

[ -f "$PLUGIN" ] || { echo "Error: plugin not found $PLUGIN"; exit 1; }

# WARNING when Wireshark is launched from the Dock/Finder its PATH is only the system directories, so the plugin's
#    `#!/usr/bin/env python3` resolves to the system python3 (macOS ships 3.9), not the one in your shell. Both the
#    dependency check and the self-test must run in this environment, otherwise everything passes in the shell but
#    Wireshark lists no interfaces at all.
GUI_PATH="/usr/bin:/bin:/usr/sbin:/sbin"
gui_env() { env -i PATH="$GUI_PATH" HOME="$HOME" "$@"; }

# Dependency check: the plugin relies on pyserial to open the serial port
if ! gui_env python3 -c "import serial" 2>/dev/null; then
	echo "⚠️  pyserial is missing, so the plugin won't run even once installed. Run first:"
	echo "      $(gui_env sh -c 'command -v python3') -m pip install --user pyserial"
	echo
fi

mkdir -p "$EXTCAP_DIR"
# Remove copies left behind by the pre-rename names (nrf_sniffer_extcap.py / tri_aggregator.py), otherwise Wireshark lists the plugin twice.
rm -f "$EXTCAP_DIR/nrf_sniffer_extcap.py" "$EXTCAP_DIR/nrf_sniffer_extcap.bat" "$EXTCAP_DIR/tri_aggregator.py"
cp "$PLUGIN" "$EXTCAP_DIR/$PLUGIN_NAME"
# The three-way merge logic lives in blehound_tri_aggregator.py in the same directory (the main script imports it), so it must
# be installed alongside, otherwise Wireshark hits an ImportError when calling --extcap-interfaces and lists no interfaces at all.
cp "$(dirname "$PLUGIN")/blehound_tri_aggregator.py" "$EXTCAP_DIR/blehound_tri_aggregator.py"
chmod +x "$EXTCAP_DIR/$PLUGIN_NAME"
echo "Installed -> $EXTCAP_DIR/$PLUGIN_NAME"

# Post-install self-test: run the plugin directly to see whether it can find the device.
#
# Note don't judge by `tshark -D` -- in practice the headless Homebrew wireshark (4.6.7) only scans the
# extcap directory and never actually invokes the tools inside it (a shell-script probe is likewise not executed),
# so its failure to list an extcap interface does not mean the plugin is broken. Go by the Wireshark GUI's interface list.
if ! out="$(gui_env "$EXTCAP_DIR/$PLUGIN_NAME" --extcap-interfaces 2>&1)"; then
	echo "❌ Self-test failed: the plugin does not run in Wireshark's runtime environment"
	echo "   (system python3 = $(gui_env python3 --version 2>&1); Wireshark uses it, not the $(python3 --version 2>&1) in your shell)"
	echo "$out" | tail -n 5 | sed 's/^/   | /'
	exit 1
fi
if echo "$out" | grep -q "^interface "; then
	echo "✅ Self-test passed: the plugin runs and a sniffer device was found"
else
	echo "ℹ️  The plugin is installed, but no sniffer device is found right now. Please check:"
	echo "   · the USB cable is plugged into the port labeled 'nRF USB' (not the debug port)"
	echo "   · the firmware has been flashed (tools/flash.sh)"
fi

# ---- Also install Wireshark display-filter bookmarks (commonly used for BLE captures) ----
# dfilters lives at ~/.config/wireshark/dfilters (not in the extcap directory) and is read by Wireshark at startup.
# If bookmarks already exist, merge and dedup (by name) without overwriting the user's own; run this script after switching machines to bring this filter set along.
DFILTERS_SRC="$(dirname "$PLUGIN")/wireshark_dfilters"
DFILTERS_DST="$HOME/.config/wireshark/dfilters"
if [ -f "$DFILTERS_SRC" ]; then
	mkdir -p "$(dirname "$DFILTERS_DST")"
	if [ -f "$DFILTERS_DST" ]; then
		cp "$DFILTERS_DST" "$DFILTERS_DST.bak.$(date +%Y%m%d%H%M%S)"
		# Merge: existing entries first, then append any name from the source that hasn't appeared
		awk -F'"' 'NR==FNR{if($2!="")seen[$2]=1; print; next} {if($2!="" && !seen[$2]) print}' 			"$DFILTERS_DST" "$DFILTERS_SRC" > "$DFILTERS_DST.tmp" && mv "$DFILTERS_DST.tmp" "$DFILTERS_DST"
		echo "✅ Merged Wireshark filter bookmarks -> $DFILTERS_DST (original file backed up)"
	else
		cp "$DFILTERS_SRC" "$DFILTERS_DST"
		echo "✅ Installed Wireshark filter bookmarks -> $DFILTERS_DST"
	fi
	echo "   (if Wireshark is open, restart it to load them; use them via the bookmark icon on the left of the filter bar)"
fi

echo
echo "== Done. Next:"
echo "   1) Plug the USB cable into the board's port labeled 'nRF USB'"
echo "   2) Restart Wireshark"
echo "   3) Select 'BLEhound Sniffer' in the interface list and double-click to start capturing"
