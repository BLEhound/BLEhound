> **English** | [中文](README.zh-CN.md)

# BLEhound hardware

Open hardware for the BLEhound sniffer: a USB dongle around the **nRF54LM20A** SoC
with an **nRF21540** front-end module (PA/LNA) for improved range and sensitivity.
Licensed under [CERN-OHL-S-2.0](LICENSE).

## Versions

| Folder | What | Fabricate? |
|---|---|---|
| [`v1/`](v1/) | **The only fabricated board so far** (JLCEDA 0908) — Gerbers, BOM, pick-and-place, assembly, JLCEDA Pro source | ⚠️ Has known defects — three-board mode needs rework (single-board OK) |
| [`v2-wip/`](v2-wip/) | Revision that folds V1's fixes into the PCB — **routing not finished** | ❌ Do not fabricate |
| [`case/`](case/) | 3D-printed enclosure (OpenSCAD + STL + sliced 3MF) — version-independent | ✅ Shared by both |

> **Which board actually works?** A plain V1 works for **single-board sniffing only**.
> Its first batch has two pin-level defects that break the **three-board** mode. For
> working three-board hardware, **wait for the V2 revision** (not yet routed) or use a
> **reworked V1** (flying-wire fixes applied, or a modified V1 unit). Details and the
> wiring diagram: [`v1/README.md`](v1/README.md).

## Board at a glance

- **SoC:** nRF54LM20A (BLE 5.x/6.x). The firmware also runs on nRF52840.
- **Front-end:** nRF21540 FEM (PA + LNA) — better link budget than bare-radio sniffers.
- **Host:** USB CDC serial to the PC / Wireshark.
- **Multi-board:** per-board SWD header, plus a SYNC line + inter-board SPI so three
  boards can be time-aligned for synchronized multi-channel capture.

## Why two versions

The first batch (V1, 0908) was fabricated but has two pin-level defects that break the
three-board synchronized mode (single-board sniffing is fine). A plain V1 therefore is
**not** a working three-board board — it needs flying-wire rework on the debug headers
(see [`v1/README.md`](v1/README.md)). **V2** integrates those fixes directly into the
PCB so no rework is needed, but its routing is not yet complete ([`v2-wip/`](v2-wip/),
do not fabricate). **So for working three-board hardware: wait for V2, or use a
reworked V1.**
