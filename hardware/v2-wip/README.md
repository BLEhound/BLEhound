> **English** | [中文](README.zh-CN.md)

# BLEhound hardware — V2 (work in progress, DO NOT fabricate)

⚠️ **This is an unfinished revision — routing is incomplete. Do not fabricate it.**
V2 is the *fix* for V1's pin-level defects; until it is finished, working three-board
hardware means a **reworked V1** (flying-wire fixes applied) — see
[`../v1/README.md`](../v1/README.md). A plain V1 works for single-board sniffing only.

## What V2 changes (vs V1)

V2 folds V1's fixes directly onto the PCB, so **no flying wires are needed**. For SYNC
and the straps the firmware pin targets are unchanged — V1 reaches them by flying wire,
V2 by native routing. Concretely:

| Signal | V1 (0908, as fabricated) | V2 | Why |
|---|---|---|---|
| **SYNC** in / out | P2.01, one shared net — **no GPIOTE**, edge capture fails; reached by flywire | **P0.04** (in) / **P0.03** (out), routed on the PCB | P2 has no GPIOTE; capture must live on a P0/P1 pin |
| **Role straps** | mis-wired (roles A/C collide, no board guards ch37); reached by flywire | **P1.08** (strap0) / **P1.09** (strap1), routed on the PCB | match `board_role.c`, GPIOTE-capable |
| **Inter-chip SPI** (SPIM00) | SCK=P2.02, MOSI=P2.03, MISO=P2.04 — **not** the SPIM00 fixed pins | SCK=**P2.01**, MOSI=P2.02, MISO=P2.04 (the dedicated SPIM00 pins) | SPIM00's fixed SCK is P2.01, which V1 had used for SYNC; freed once SYNC moved off it |
| **FEM SPI** (nRF21540) | CSN=P0.06, SCK=P0.07, MOSI=P0.08, MISO=P0.09 | CSN=**P1.19**, SCK=**P1.24**, MOSI=**P1.21**, MISO=**P1.20** (all on P1) | datasheet same-port rule + NCS PERI-domain `spi2x` needs CS on P1; also clears the early-sample P0.09 hazard |
| **FEM MODE / ANT_SEL** | MODE=P1.20, ANT_SEL=P1.19 | MODE=**P0.08**, ANT_SEL=**P0.06** (P0.07 freed) | follows the FEM SPI move |
| **FEM TX_EN / RX_EN / PDN** | P1.15 / P1.16 / P1.18 | unchanged | — |
| **USB hub power** (CH334F PSELF) | bus-powered, PSELF (pin 18) floating | **self-powered** (PSELF-SEL net + local 3V3 from U8 AP2112K) | three SoCs + three FEMs exceed the 100 mA bus-power default |

The SYNC/strap **firmware** pins live in `boards/common/blehound_nrf54lm20a_cpuapp_common.dtsi` (shared by both
boards). The SPIM00 and FEM moves are the `boards/blehound_v2/` board definition
(build with `tools/build.sh V2=1`).

**Status: routing NOT complete.** The schematic carries the V2 changes, but the PCB
has not been re-routed for them. The physical layout is still the V1 (0908) layout;
several nets (SYNC, straps, MISO, hub PSELF) are unrouted, and a DRC run reports
shorts / unconnected items where V2 nets were reassigned but the copper was not
redrawn yet.

`kicad/` (`blehound.kicad_*`) is the KiCad project for continuing that routing.
