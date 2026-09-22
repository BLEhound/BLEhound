> [English](README.md) | **中文**

<div align="center">

# BLEhound

**一款开放的、带 FEM 增益的 nRF54 蓝牙低功耗（BLE）嗅探器 —— 支持同步多信道抓包。**

固件 · Wireshark 主机工具 · 开放硬件

📖 完整文档：**https://blehound.github.io**

</div>

---

BLEhound 是一款完全从零构建的 BLE 嗅探器。其射频寄存器由我们自己的固件配置
（而非 Nordic 的嗅探器固件），正是这一点让连接跟随（connection following）、
加密链路抓包（encrypted-link capture）以及**三路射频同步多信道**
（three-radio synchronized multi-channel）抓包这样的深度特性成为可能。它以完整
套件的形式交付：固件、一个 Wireshark `extcap` 插件，以及开放硬件（JLCEDA 源文件、
Gerber、BOM、3D 打印外壳）。

## 与众不同之处

- **同步多信道抓包（三块板）。** 单块射频一次只能监听一个信道，因此单独一台嗅探器
  可能会漏掉落在另一个广播信道上的 `CONNECT_IND`。BLEhound 用三块板运行，各自
  把守 37 / 38 / 39，通过硬件 SYNC 线 + 板间 SPI 做时间对齐，再由主机合并成
  **一个** Wireshark 接口。已在真实硬件上端到端验证。
- **FEM（nRF21540 PA/LNA）。** 相比裸射频嗅探器有更好的灵敏度和距离。
- **新芯片，新特性。** nRF54LM20A（同样可在 nRF52840 上运行）。支持 CSA #1/#2 的
  连接跟随、1M / 2M / Coded PHY 及连接内 PHY 更新、扩展广播（`AUX_CONNECT_REQ`），
  以及 BLE 5.x/6.x 链路层覆盖。
- **兼作低成本 BLE 射频测试台。** 详见文档 —— 同一套硬件即可驱动相对/黄金参考
  （relative/golden-referenced）的量产射频检查。

## 它诚实交代的地方

- 作为**单块板**，它是一款称职的嗅探器，与优秀的
  [Sniffle](https://github.com/nccgroup/Sniffle) 和 Nordic nRF Sniffer 并驾齐驱 ——
  它在此处的优势在于 FEM、更新的芯片以及一体化硬件。
- **多板冗余**在有损/临界链路上才见其价值；链路干净时单块板就已经抓到了全部内容。
- 三板接力**不**适用于通过扩展广播（`AUX_CONNECT_REQ`）建立的连接，也无法恢复加密的
  控制 PDU（所有板会一起漏掉同一个 `instant`）。

## 仓库结构

| 路径 | 内容 |
|---|---|
| [`firmware/`](firmware/) | Zephyr / nRF Connect SDK 应用（C）。构建与烧录：[firmware/README.zh-CN.md](firmware/README.zh-CN.md) |
| [`host/`](host/) | Wireshark `extcap` 插件 + 多信道聚合器（Python）。用法：[host/README.zh-CN.md](host/README.zh-CN.md) |
| [`hardware/`](hardware/) | 开放硬件。**V1** = 已投产板(JLCEDA 源 + Gerber/BOM/外壳);**V2** = 在制。详情见 [hardware/README.zh-CN.md](hardware/README.zh-CN.md) —— 注意 V1 已知问题(单片可用;三机需返工)。 |
| [`tools/`](tools/) | 构建 / 烧录 / RTT / extcap 安装辅助脚本 |
| [`west.yml`](west.yml) | west manifest（从 Nordic 官方 GitHub 拉取 NCS） |

## 快速开始

```bash
# 1) 固件（在一个空的工作区目录中）
git clone https://github.com/BLEhound/BLEhound
west init -l BLEhound && west update && west zephyr-export
west build -b nrf54lm20dk/nrf54lm20a/cpuapp -s BLEhound/firmware \
  -- -DEXTRA_CONF_FILE=boards/blehound.conf \
     -DEXTRA_DTC_OVERLAY_FILE=boards/blehound.overlay
west flash            # or: BLEhound/tools/flash.sh

# 2) 主机插件
BLEhound/tools/install_extcap.sh      # copies the extcap into Wireshark
# then open Wireshark → interface "nRF BLE Sniffer"
```

完整操作流程（单板与三板）：**https://blehound.github.io**

## 许可

- **代码**（固件 + 主机）：[Apache-2.0](LICENSE)
- **硬件**（原理图 / PCB / Gerber）：[CERN-OHL-S-2.0](hardware/LICENSE)
- 第三方组件（Zephyr、nRF Connect SDK、nrfxlib）保留各自的许可。

## 法律 / 道德使用

BLEhound 会抓取射频流量，并包含传统配对（legacy-pairing）分析工具（等同于
`crackle`）。仅可用于你自己拥有或已获明确授权测试的设备。请注意
**LE 安全连接（LE Secure Connections，BLE 4.2+）无法被被动破解** —— 任何嗅探器
都做不到。参见 [host/security/README.zh-CN.md](host/security/README.zh-CN.md)。

## 参与贡献

参见 [CONTRIBUTING.md](CONTRIBUTING.md)。欢迎提交 Issue 与 PR —— 尤其是多信道路径，
在更多中心/外围（central/peripheral）协议栈上测试将大有裨益。
