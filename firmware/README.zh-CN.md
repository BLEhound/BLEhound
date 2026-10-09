> [English](README.md) | **中文**

# BLEhound 固件

一款基于 nRF Connect SDK（Zephyr）从零构建的 BLE 嗅探器。射频由固件直接驱动
（见 `src/radio_nrf54l.c` / `src/radio_nrf52.c`），而非使用 Nordic 的嗅探器固件，
正是这一点让连接跟随（connection following）、加密抓包（encrypted capture）以及
三板同步多信道抓包成为可能。

- **目标芯片：** nRF54LM20A（主用，搭配 nRF21540 FEM）与 nRF52840。
- **SDK：** nRF Connect SDK **v3.4.0**（在`firmware/blehound/west.yml` 中锁定）。

## 工具链搭建（west）

```bash
# 克隆仓库（路径不要含空格），在 firmware/ 内搭建自包含工作区：
git clone https://github.com/BLEhound/BLEhound
cd BLEhound/firmware
west init -l blehound        # firmware/blehound/west.yml 是 manifest
west update                  # 从 Nordic 官方 GitHub 拉取 NCS(nrf/zephyr/...)到 firmware/ 下
west zephyr-export
```

> **Windows:** 用 **nRF Connect for Desktop → Toolchain Manager** 装工具链(选 NCS v3.4.0),在它自带的命令行里跑上面这些 `west` 命令——命令在 macOS / Linux / Windows 上完全一致。

你还需要一个 **Zephyr SDK 1.0.1**（配 NCS v3.4.0）。nRF Connect Toolchain Manager 会自动
注册它；**手动解压的 SDK 必须先注册一次**，用 CMake：

```bash
<sdk目录>/setup.sh -c      # 或: cmake -P <sdk目录>/cmake/zephyr_sdk_export.cmake
```

或导出 `ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1`。否则构建会报
*"Could not find a package configuration file provided by Zephyr-sdk"*。

> **已经有 NCS v3.4.0 工作区?** 不必重新下载 —— 跳过 `west init` / `west update`,直接把构建脚本指向现成的 NCS:`NCS_TOPDIR=/path/to/ncs tools/build.sh`。

## 构建

```bash
# 在 firmware/ 下执行。板定义在 firmware/blehound/boards/blehound_v{1,2}/,
# 经 blehound/zephyr/module.yml 自动发现,无需 -DBOARD_ROOT。
west build -b blehound_v1/nrf54lm20a/cpuapp blehound/projects/blehound --sysbuild
# 改版板(V2): -b blehound_v2/nrf54lm20a/cpuapp
```

或使用便捷封装脚本（自动选择 overlay 和输出目录）：

```bash
tools/build.sh                          # nRF54LM20A dongle → build/
```

## 烧录

```bash
west flash
# or, via nrfutil + J-Link (handles APPROTECT recovery):
tools/flash.sh                 # auto-detects the J-Link and hex
RECOVER=1 tools/flash.sh       # recover (unlock APPROTECT) first
```

每块板烧录的都是同一份固件；三板方案下，把三块都烧上，并连接你正在编程的那块板的
SWD 排针。

构建是 sysbuild 三镜像（MCUboot + USB 固件加载器 + 签名应用，布局见
`firmware/blehound/boards/common/blehound_partitions.dtsi`）。产物统一以固定名字收在 `build/blehound/`：
`blehound_mcu_boot.*`、`blehound_loader.*`、`blehound_app.*`（各有 bin/hex/elf/map）、
`blehound_ota.bin` / `blehound_ota.zip`（DFU 上传用）和 `blehound_merged.hex`（整片，J-Link 烧这个）。
之后升级不再需要 J-Link：

```bash
tools/dfu.sh /dev/cu.usbmodemXXXX      # 对一块板做 USB DFU（默认用 build/blehound/blehound_ota.bin）
tools/dfu.sh all                       # 依次升级每块板
tools/verify_flash.sh <J-Link SN>      # 逐字节比对片上三个分区
```

`dfu.sh` 往抓包口发 `HOST_CMD_ENTER_DFU`，板子重新枚举为 "BLEhound Loader"（PID 0x5210），
再由 `nrfutil mcu-manager` 经 SMP 上传镜像。BLEhound Analyzer 内置了同样的流程
（设备面板 →「升级固件…」）。

**签名密钥。** MCUboot 只启动用「公钥已编进它」的那把 key 签的镜像。`tools/keygen.sh`
在仓库外生成 ed25519 密钥 `~/.blehound/keys/blehound_ed25519.pem`（切勿入库），并把公钥导出到
`firmware/blehound/projects/blehound/keys/blehound_ed25519.pub.pem`；`tools/build.sh` 会自动使用私钥（也可用
`BLEHOUND_SIGNING_KEY` 指定）。没有 key 时回落到 MCUboot 公开的开发 key 并告警：本地调试可以，
不能发布。换 key 会改变 MCUboot，老板子要先用 J-Link 重烧一次 `blehound_merged.hex` 才认新签名。

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
