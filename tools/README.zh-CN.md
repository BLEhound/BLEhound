> [English](README.md) | **中文**

# tools

用于构建、烧录和调试 BLEhound 的辅助脚本。

| 脚本 | 用途 |
|---|---|
| `build.sh` | 对 `west build` 的封装（选择板卡目标 + overlay，输出到各自独立的目录） |
| `flash.sh` | 通过现代 `nrfutil` 经 J-Link 烧录（处理 APPROTECT 恢复） |
| `flash_jlink.sh` | 通过 SEGGER J-Link 工具烧录（若不在 `/opt/SEGGER/JLink`，设置 `JLINK_DIR`） |
| `jlink_common.sh` | 共享的 J-Link 辅助函数（被烧录脚本 source 引用） |
| `recover.sh` | 恢复 / 解锁被锁定的设备（APPROTECT） |
| `install_extcap.sh` | 把 Wireshark extcap 插件复制到 Wireshark 的 extcap 目录 |
| `rtt.sh` | 挂接 SEGGER RTT 以查看固件日志 |
| `verify_flash.sh` | 校验已烧录的镜像与构建出的 hex 是否一致 |
| `cdc_test.sh` | 对嗅探器串口做快速的 USB-CDC 健全性检查 |

多数脚本会读取可覆盖的环境变量（`NCS_TOPDIR`、`ZEPHYR_SDK_INSTALL_DIR`、
`JLINK_DIR`、`SN`、`DEVICE`……）。不带参数运行脚本可查看其默认值，或阅读文件头部的
注释。
