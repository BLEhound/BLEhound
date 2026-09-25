> **English** | [中文](README.zh-CN.md)

# BLEhound host tools

Python tools that turn the sniffer's USB stream into Wireshark captures.

- `blehound_extcap.py` — the Wireshark [extcap](https://www.wireshark.org/docs/man-pages/extcap.html)
  plugin. Wireshark calls it to list interfaces/options and to stream packets.
  It opens the sniffer's USB serial port, de-frames (COBS), and writes PCAP
  (`LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR`) into the Wireshark fifo.
- `blehound_extcap.bat` — Windows wrapper: Wireshark on Windows runs `.exe`/`.bat`
  extcap programs, not `.py` directly, so this launches the plugin via Python.
- `blehound_tri_aggregator.py` — pure-logic module that time-aligns and merges three boards
  into one stream (`SyncClock` + `Aggregator` + `FollowRelay`). Runnable self-test:
  `python3 blehound_tri_aggregator.py --selftest`.
- `wireshark_dfilters` — a set of handy Wireshark display-filter bookmarks.
- `security/` — legacy-pairing analysis and decryption helpers (see its README).

## Requirements

Python 3.9+ (Wireshark uses the system `python3`; on macOS that is 3.9).

```bash
pip install -r requirements.txt      # pyserial (+ cryptography for security/)
```

## Install into Wireshark

```bash
../tools/install_extcap.sh           # copies the extcap into Wireshark's extcap dir
```

Or manually:

```bash
# Wireshark 4.x path (older <4: ~/.config/wireshark/extcap)
mkdir -p ~/.local/lib/wireshark/extcap
cp blehound_extcap.py blehound_tri_aggregator.py ~/.local/lib/wireshark/extcap/
chmod +x ~/.local/lib/wireshark/extcap/blehound_extcap.py
```

Restart Wireshark. The interface list will show **BLEhound Sniffer** (single board)
and **BLEhound Sniffer (3ch aggregated)** (three-board merged).

### Windows

Wireshark on Windows runs `.exe` / `.bat` extcap programs, not `.py` directly, so the
bundled `.bat` wrapper is required:

1. Install Python 3 (tick **Add python.exe to PATH**) and run `pip install pyserial`.
2. Find the extcap folder in Wireshark: **Help → About Wireshark → Folders → Personal
   Extcap path** (usually `%APPDATA%\Wireshark\extcap`).
3. Copy **`blehound_extcap.py`, `blehound_extcap.bat`, and `blehound_tri_aggregator.py`**
   into that folder, then restart Wireshark.

The sniffer's serial ports appear as `COMx`; everything else works the same. If
`python` is not on PATH, edit the `.bat` to use `py -3`. The `tools/*.sh` scripts are
macOS/Linux only (use Git Bash or WSL if you want them on Windows).

The USB identity the firmware advertises: VID `0x1915`, PID `0x520F`,
product string `BLEhound Sniffer`.

## Single board vs three boards

- **Single board:** pick the `BLEhound Sniffer` interface. Set the scan channel or a
  target MAC in the interface options.
- **Three boards:** pick the **BLEhound Sniffer (3ch aggregated)** interface (optionally enable the
  three-board relay). Each board guards 37 / 38 / 39; the aggregator merges and
  de-duplicates into one timeline. See the docs for wiring the SYNC line.

Filter bookmarks: load `wireshark_dfilters` entries to quickly isolate a target
device, connection data, CONNECT_IND, CRC errors, etc. Replace the example address
`aa:bb:cc:dd:ee:ff` with your device's address.
