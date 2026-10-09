# BLEhound — VS Code workspace config (.vscode)

One-click build / flash / RTT for the BLEhound firmware, cross-platform (macOS / Windows / Linux).

## Layout (self-contained west workspace)

    BLEhound/                    <- open this folder in VS Code
    ├── .vscode/                 <- this directory
    ├── firmware/                <- west workspace root (west topdir)
    │   ├── .west/               <- west init output, not committed
    │   ├── blehound/            <- west manifest repo + Zephyr module
    │   │   ├── west.yml         NCS v3.4.0 manifest
    │   │   ├── zephyr/module.yml   board_root -> boards/
    │   │   ├── boards/          board defs (blehound_v1 / v2 / common)
    │   │   └── projects/blehound/  application (CMakeLists + prj.conf + sysbuild* + src/)
    │   └── nrf/ zephyr/ nrfxlib/ modules/ bootloader/   <- west update output, not committed
    └── host/ hardware/ tools/ docs/

All NCS deps are pulled under `firmware/`; nothing outside the repo is required.

## First-time setup (run the script BEFORE opening VS Code)

    git clone https://github.com/BLEhound/BLEhound     # keep the path free of spaces
    bash .vscode/blehound_west_init.sh                 # Windows: blehound_west_init.bat
    # == cd firmware && west init -l blehound && west update && west zephyr-export
    code .

## Tasks (Terminal -> Run Task)

| Task | Action |
|------|--------|
| **[01] Build · V1** | build blehound_v1 (default build) |
| [02] Build · V1 pristine | full rebuild |
| [04] Build · V2 | build the revised board |
| **[11] Flash · whole-chip** | erase + program + reset (J-Link) |
| [12] Flash · recover then flash | unlock APPROTECT first |
| [13] Recover | mass-erase / unlock only |
| [14] Build + Flash | both, in sequence |
| **[41/42] RTT log** | view the firmware RTT log |
| [31] west update | sync NCS deps |

Command-line equivalents live in `tools/` (`build.sh` / `flash_jlink.sh` / `rtt.sh` / ...).
With several boards attached, pass `--sn <J-Link serial>`.

## Environment

`west`, the Zephyr SDK and SEGGER J-Link must be on PATH (this config injects no
machine-specific paths). Pairs with **NCS v3.4.0** + **Zephyr SDK 1.0.1**. The build
auto-detects the SDK at `~/zephyr-sdk-<ver>` or `/opt/zephyr-sdk-<ver>`, or honours
`ZEPHYR_SDK_INSTALL_DIR`. `ZEPHYR_BASE` is left unset (west locates Zephyr itself).

## Hardware

- **BLEhound V1 / V2 (nRF54LM20A)** — debug via J-Link + RTT; USB CDC/DFU for capture and updates.
