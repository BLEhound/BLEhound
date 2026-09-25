> [English](README.md) | **中文**

# BLEhound 硬件 —— V2(在制,请勿投产)

⚠️ **这是未完成的改版,布线不完整,不要拿去投产。** V2 是对 V1 引脚级缺陷的*修正*;在它完成前,可用的三机硬件只能是**改过的 V1**(飞线修正)——见 [`../v1/README.zh-CN.md`](../v1/README.zh-CN.md)。原始 V1 只有单片抓包可用。

## V2 相对 V1 改了什么

V2 把 V1 的修正直接做进 PCB,**从而免飞线**。SYNC 和 strap 的固件目标引脚不变——V1 靠飞线到达,V2 靠 PCB 布线到达。具体:

| 信号 | V1(0908 实产) | V2 | 原因 |
|---|---|---|---|
| **SYNC** 入 / 出 | P2.01,三片共一条网——**无 GPIOTE**,边沿捕获失效;靠飞线 | **P0.04**(入)/ **P0.03**(出),PCB 直连 | P2 没有 GPIOTE,捕获必须落在 P0/P1 |
| **角色 strap** | 接法错(A/C 角色冲突、无一片守 ch37);靠飞线 | **P1.08**(strap0)/ **P1.09**(strap1),PCB 直连 | 对上 `board_role.c`、且有 GPIOTE |
| **片间 SPI**(SPIM00) | SCK=P2.02、MOSI=P2.03、MISO=P2.04——**不是** SPIM00 的固定脚 | SCK=**P2.01**、MOSI=P2.02、MISO=P2.04(SPIM00 专用脚) | SPIM00 固定 SCK 是 P2.01,V1 拿去做了 SYNC;SYNC 挪走后释放出来 |
| **FEM SPI**(nRF21540) | CSN=P0.06、SCK=P0.07、MOSI=P0.08、MISO=P0.09 | CSN=**P1.19**、SCK=**P1.24**、MOSI=**P1.21**、MISO=**P1.20**(全挪到 P1) | 数据手册同口规则 + NCS PERI 域 `spi2x` 要求 CS 在 P1;顺带避开早期样片 P0.09 隐患 |
| **FEM MODE / ANT_SEL** | MODE=P1.20、ANT_SEL=P1.19 | MODE=**P0.08**、ANT_SEL=**P0.06**(P0.07 空出) | 随 FEM SPI 挪位 |
| **FEM TX_EN / RX_EN / PDN** | P1.15 / P1.16 / P1.18 | 不变 | — |
| **USB hub 供电**(CH334F PSELF) | 总线供电,PSELF(18 脚)悬空 | **自供电**(PSELF-SEL 网 + U8 AP2112K 本地 3V3) | 三颗 SoC + 三颗 FEM 超过 100 mA 总线供电默认额度 |

SYNC/strap 的**固件**引脚在 `boards/common/blehound_nrf54lm20a_cpuapp_common.dtsi` 里(两版共用);SPIM00 和 FEM 的挪位是 `boards/blehound_v2/` 板定义(`tools/build.sh V2=1` 构建)。

**状态:布线未完成。** 原理图已带 V2 改动,但 PCB 尚未按其重新布线。物理布局仍是 V1(0908)那版;若干网络(SYNC、strap、MISO、hub PSELF)未布线,跑 DRC 会在"改了网但没重画铜"的地方报短路 / 未连接。

`kicad/`(`blehound.kicad_*`)就是用来继续布线的 KiCad 工程。
