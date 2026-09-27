> **English** | [中文](README.zh-CN.md)

# BLEhound firmware

A from-scratch BLE sniffer built on the nRF Connect SDK (Zephyr). The radio is
driven directly (see `src/radio_nrf54l.c` / `src/radio_nrf52.c`) rather than using
Nordic's sniffer firmware, which is what enables connection following, encrypted
capture, and three-board synchronized multi-channel capture.

- **Targets:** nRF54LM20A (primary, with nRF21540 FEM) and nRF52840.
- **SDK:** nRF Connect SDK **v3.4.0** (pinned in the top-level `west.yml`).

## Toolchain setup (west)

```bash
# From an empty workspace directory (its path must contain no spaces):
git clone https://github.com/BLEhound/BLEhound
west init -l BLEhound        # BLEhound/west.yml is the manifest
west update                  # pulls NCS (nrf/zephyr/...) from official Nordic GitHub
west zephyr-export
```

> **Windows:** get the toolchain from **nRF Connect for Desktop → Toolchain Manager**
> (install NCS v3.4.0) and run these `west` commands in its bundled command prompt —
> they are identical across macOS, Linux, and Windows.

You also need a Zephyr SDK compatible with NCS v3.4.0 (1.0.1). Point to it with
`ZEPHYR_SDK_INSTALL_DIR` if it is not auto-detected.

> **Already have an NCS v3.4.0 workspace?** No need to re-download it — skip `west init`
> / `west update` and point the build wrapper at your existing NCS:
> `NCS_TOPDIR=/path/to/ncs BLEhound/tools/build.sh`.

## Build

```bash
# BLEhound V1 (first-batch board); board definitions live in firmware/boards/blehound_v{1,2}/
west build -b blehound_v1/nrf54lm20a/cpuapp -s BLEhound/firmware -- -DBOARD_ROOT=.
# revised board (V2): -b blehound_v2/nrf54lm20a/cpuapp
```

Or use the convenience wrapper (auto-selects overlays and output dir):

```bash
BLEhound/tools/build.sh                          # nRF54LM20A dongle → build_dongle/
```

## Flash

```bash
west flash
# or, via nrfutil + J-Link (handles APPROTECT recovery):
BLEhound/tools/flash.sh                 # auto-detects the J-Link and hex
RECOVER=1 BLEhound/tools/flash.sh       # recover (unlock APPROTECT) first
```

Each board takes the same firmware; for a three-board setup, flash all three and
connect the SWD header of whichever board you are programming.

The build is a sysbuild with three images (MCUboot + USB firmware loader + signed app,
layout in `boards/common/blehound_partitions.dtsi`). All outputs are collected with fixed
names in `build_dongle/blehound/`: `blehound_mcu_boot.*`, `blehound_loader.*`, `blehound_app.*`
(bin/hex/elf/map each), `blehound_ota.bin` / `blehound_ota.zip` (DFU upload) and
`blehound_merged.hex` (whole chip, what J-Link programs). After that, updates need no J-Link:

```bash
BLEhound/tools/dfu.sh /dev/cu.usbmodemXXXX      # USB DFU of one board (default: build_dongle/blehound/blehound_ota.bin)
BLEhound/tools/dfu.sh all                       # every board in turn
BLEhound/tools/verify_flash.sh <J-Link SN>      # byte-compare all three regions on the chip
```

`dfu.sh` sends `HOST_CMD_ENTER_DFU` on the capture port, the board re-enumerates as
"BLEhound Loader" (PID 0x5210) and `nrfutil mcu-manager` uploads the image over SMP.
BLEhound Analyzer has the same flow built in (device panel → "Update firmware…").

**Signing key.** MCUboot only boots images signed with the key whose public half is
compiled into it. `tools/keygen.sh` creates an ed25519 key at
`~/.blehound/keys/blehound_ed25519.pem` (outside the repo, never commit it) and exports
the public key to `firmware/keys/blehound_ed25519.pub.pem`; `tools/build.sh` picks the
private key up automatically (or set `BLEHOUND_SIGNING_KEY`). Without a key the build
falls back to MCUboot's public development key and warns: fine for local debugging,
not for release. Changing the key changes MCUboot, so existing boards need one J-Link
programming of `blehound_merged.hex` before they accept images signed with the new key.

## Configuration (Kconfig)

| Option | Meaning |
|---|---|
| `SNIFFER_DEFAULT_TARGET_MAC` | Boot-time target MAC (empty = no filter). Overridden by the host once connected. Format `aa:bb:cc:dd:ee:ff`. |
| `TRI_STRATEGY_*` | Three-board follow strategy — how the boards split work after a `CONNECT_IND`. |

At runtime the host (extcap) controls channel, target MAC, single/multi-target mode,
and multi-channel relay.

## Layout

- `src/radio_*.c` — direct radio driver per SoC
- `src/conn_follower.c` — connection following (CSA #1/#2, PHY update, param update)
- `src/adv_filter.*` — advertising / target-MAC filtering
- `src/sync_line.c`, `src/peer_link.c`, `src/tri_coord.c`, `src/board_role.c` — three-board time sync and coordination
- `src/host_iface.*`, `src/cobs.*` — USB host framing (COBS)
- `src/fem_ctrl.*` — nRF21540 FEM control

> Note on comments: some source comments cite an internal design document by section
> number (e.g. "design §5.1", "scheme §4.5"). That document is not part of this
> release; each module's header comment summarizes the design it needs, so the code is
> self-contained without it.
