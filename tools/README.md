> **English** | [中文](README.zh-CN.md)

# tools

Helper scripts for building, flashing, and debugging BLEhound.

| Script | Purpose |
|---|---|
| `build.sh` | Wrapper around `west build` (selects board target + overlays, separate output dirs) |
| `flash.sh` | Flash via modern `nrfutil` over J-Link (handles APPROTECT recovery) |
| `flash_jlink.sh` | Flash via the SEGGER J-Link tools (set `JLINK_DIR` if not at `/opt/SEGGER/JLink`) |
| `jlink_common.sh` | Shared J-Link helpers (sourced by the flash scripts) |
| `recover.sh` | Recover / unlock a locked device (APPROTECT) |
| `install_extcap.sh` | Copy the Wireshark extcap plugin into Wireshark's extcap directory |
| `rtt.sh` | Attach to SEGGER RTT for firmware logs |
| `verify_flash.sh` | Verify the flashed image matches the built hex |
| `cdc_test.sh` | Quick USB-CDC sanity check of the sniffer serial port |

Most scripts read overridable environment variables (`NCS_TOPDIR`,
`ZEPHYR_SDK_INSTALL_DIR`, `JLINK_DIR`, `SN`, `DEVICE`, …). Run a script with no
arguments to see its defaults, or read the header comment.
