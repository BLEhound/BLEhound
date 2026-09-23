> [English](README.md) | **中文**

# BLEhound 固件

一款基于 nRF Connect SDK（Zephyr）从零构建的 BLE 嗅探器。射频由固件直接驱动
（见 `src/radio_nrf54l.c` / `src/radio_nrf52.c`），而非使用 Nordic 的嗅探器固件，
正是这一点让连接跟随（connection following）、加密抓包（encrypted capture）以及
三板同步多信道抓包成为可能。

- **目标芯片：** nRF54LM20A（主用，搭配 nRF21540 FEM）与 nRF52840。
- **SDK：** nRF Connect SDK **v3.4.0**（在顶层 `west.yml` 中锁定）。

## 工具链搭建（west）

```bash
# From an empty workspace directory (its path must contain no spaces):
git clone https://github.com/BLEhound/BLEhound
west init -l BLEhound        # BLEhound/west.yml is the manifest
west update                  # pulls NCS (nrf/zephyr/...) from official Nordic GitHub
west zephyr-export
```

> **Windows:** 用 **nRF Connect for Desktop → Toolchain Manager** 装工具链(选 NCS v3.4.0),在它自带的命令行里跑上面这些 `west` 命令——命令在 macOS / Linux / Windows 上完全一致。

你还需要一个与 NCS v3.4.0 兼容的 Zephyr SDK（1.0.1）。若未被自动检测到，请用
`ZEPHYR_SDK_INSTALL_DIR` 指向它。

> **已经有 NCS v3.4.0 工作区?** 不必重新下载 —— 跳过 `west init` / `west update`,直接把构建脚本指向现成的 NCS:`NCS_TOPDIR=/path/to/ncs BLEhound/tools/build.sh`。

## 构建

```bash
# nRF54LM20A BLEhound (default board target)
west build -b nrf54lm20dk/nrf54lm20a/cpuapp -s BLEhound/firmware \
  -- -DEXTRA_CONF_FILE=boards/blehound.conf \
     -DEXTRA_DTC_OVERLAY_FILE=boards/blehound.overlay

# nRF52840 DK / Dongle
west build -b nrf52840dk/nrf52840 -s BLEhound/firmware
```

或使用便捷封装脚本（自动选择 overlay 和输出目录）：

```bash
BLEhound/tools/build.sh                          # nRF54LM20A dongle → build_dongle/
BLEHOUND=0 BLEhound/tools/build.sh nrf52840dk/nrf52840 # nRF52840 → build/
```

## 烧录

```bash
west flash
# or, via nrfutil + J-Link (handles APPROTECT recovery):
BLEhound/tools/flash.sh                 # auto-detects the J-Link and hex
RECOVER=1 BLEhound/tools/flash.sh       # recover (unlock APPROTECT) first
```

每块板烧录的都是同一份固件；三板方案下，把三块都烧上，并连接你正在编程的那块板的
SWD 排针。

## 配置（Kconfig）

| 选项 | 含义 |
|---|---|
| `SNIFFER_DEFAULT_TARGET_MAC` | 启动时的目标 MAC（留空 = 不过滤）。一旦连上主机即被其覆盖。格式为 `aa:bb:cc:dd:ee:ff`。 |
| `TRI_STRATEGY_*` | 三板跟随策略 —— 收到 `CONNECT_IND` 后各板如何分工。 |

运行时，由主机（extcap）控制信道、目标 MAC、单/多目标模式以及多信道接力。

## 代码结构

- `src/radio_*.c` —— 各 SoC 的直接射频驱动
- `src/conn_follower.c` —— 连接跟随（CSA #1/#2、PHY 更新、参数更新）
- `src/adv_filter.*` —— 广播 / 目标 MAC 过滤
- `src/sync_line.c`、`src/peer_link.c`、`src/tri_coord.c`、`src/board_role.c` —— 三板时间同步与协调
- `src/host_iface.*`、`src/cobs.*` —— USB 主机成帧（COBS）
- `src/fem_ctrl.*` —— nRF21540 FEM 控制

> 关于注释：部分源码注释以章节号引用了一份内部设计文档（如「design §5.1」「scheme §4.5」）。该文档不在本次发布内；每个模块的头部注释已概括其所需设计，因此不依赖该文档也能读懂代码。
