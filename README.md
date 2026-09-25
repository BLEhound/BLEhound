> **English** | [中文](README.zh-CN.md)

<div align="center">

# BLEhound

**An open, FEM-boosted Bluetooth LE sniffer on nRF54 — with synchronized multi-channel capture.**

Firmware · Wireshark host tools · open hardware

<img src="hardware/case/case_exploded.gif" width="520" alt="BLEhound — exploded view of the 3D-printed case: lid, PCB, tray">

📖 Full documentation: **https://blehound.github.io**

</div>

---

BLEhound is a from-scratch BLE sniffer. The radio registers are configured by our
own firmware (not Nordic's sniffer firmware), which is what makes deep features
like connection following, encrypted-link capture, and **three-radio synchronized
multi-channel** capture possible. It ships as a complete package: firmware, a
Wireshark `extcap` plugin, and open hardware (JLCEDA Pro source, Gerbers, BOM, 3D-printed
case).

> **Built with AI.** BLEhound's hardware schematic design, and the software (firmware
> and Wireshark host tooling) development and debugging, were done entirely by AI.

## What makes it different

- **Synchronized multi-channel capture (three boards).** A single radio can only
  listen on one channel at a time, so a lone sniffer can miss a `CONNECT_IND` that
  lands on another advertising channel. BLEhound runs three boards, each guarding
  37 / 38 / 39, time-aligned over a hardware SYNC line + inter-board SPI, and merged
  by the host into **one** Wireshark interface. Validated end-to-end on real boards.
- **FEM (nRF21540 PA/LNA).** Better sensitivity and range than bare-radio sniffers.
- **New silicon, current features.** nRF54LM20A. Connection
  following with CSA #1/#2, 1M / 2M / Coded PHY with in-connection PHY updates,
  extended advertising (`AUX_CONNECT_REQ`), and BLE 5.x/6.x link-layer coverage.
- **Doubles as a low-cost BLE RF test bench.** See the docs — the same hardware can
  drive relative/golden-referenced production RF checks.

## Feature support (BLE 4.0 → 6.x)

Most mainstream link-layer features from BLE 4.0 to 6.x are covered, with an honest boundary on what any sniffer can and cannot recover:

| Feature | Status |
|---|---|
| **Advertising & connection following (BLE 4.x)** | |
| Legacy advertising capture (all PDUs) | ✓ |
| `CONNECT_IND` follow + CSA #1 hopping | ✓ |
| Channel-map update (`LL_CHANNEL_MAP_IND`) | ✓ |
| Connection-parameter update (`LL_CONNECTION_UPDATE_IND`) | ✓ |
| Peripheral latency / supervision-timeout loss detection | ✓ |
| Connection termination (`LL_TERMINATE_IND`) | ✓ |
| Encrypted-link follow (ciphertext streamed) | ✓ |
| Inject LTK → decrypt in Wireshark | ✓ |
| Legacy pairing passive crack (TK → STK) | ✓ |
| **PHY & extended advertising (BLE 5.0)** | |
| 1M PHY | ✓ |
| 2M PHY + switch (`LL_PHY_UPDATE_IND`) | ✓ |
| Coded PHY S2/S8 (auto-detect) | ✓ |
| Asymmetric PHY (per-packet within an event) | ✓ |
| CSA #2 hopping | ✓ |
| Extended-advertising AUX chains | ✓ |
| Connection via extended adv (`AUX_CONNECT_REQ`) | ✓ |
| Periodic-advertising sync (`AUX_SYNC_IND`) | ✓ |
| **LE Audio & periodic (BLE 5.1–5.4)** | |
| BIS broadcast isochronous stream | ✓ |
| CIS connected isochronous stream | ✓ |
| CIS termination (`LL_CIS_TERMINATE_IND`) | ◐ |
| Connection subrating (5.3) | ✓ |
| PAwR subevent 0 + response slots (5.4) | ✓ |
| PAwR all subevents captured | ◐ |
| PAST — periodic sync transfer | ◐ |
| **Latest features (BLE 6.0–6.2)** | |
| Frame-space negotiation (6.0) | ✓ |
| Short connection interval, down to 375 µs (6.2) | ✓ |
| Decision-based advertising filtering (6.0) | ✓ |
| Channel Sounding negotiation PDUs streamed | ✓ |
| Dedicated parsing of new 6.3 LL PDUs | ✗ |
| **Encryption & cracking boundary** | |
| Encrypted-link decryption (known LTK) | ✓ |
| LE Secure Connections key cracking | ✗ |
| Plaintext of encrypted LL control PDUs | ✗ |
| Channel Sounding ranging reconstruction | ✗ |
| RPA resolution without an IRK | ✗ |

**Legend:** ✓ verified on real hardware · ◐ code in place, hardware trigger pending · ✗ not supported / infeasible by design

## What it is honest about

- As a **single board**, it is a capable sniffer alongside the excellent
  [Sniffle](https://github.com/nccgroup/Sniffle) and the Nordic nRF Sniffer — its
  edge there is the FEM, newer silicon, and integrated hardware.
- The **multi-board redundancy** pays off on lossy/marginal links; on a clean link a
  single board already captures everything.
- The three-board relay does **not** apply to connections established via extended
  advertising (`AUX_CONNECT_REQ`), and cannot recover encrypted control PDUs (all
  boards miss the same `instant` together).

## Repository layout

| Path | What |
|---|---|
| [`firmware/`](firmware/) | Zephyr / nRF Connect SDK application (C). Build & flash: [firmware/README.md](firmware/README.md) |
| [`host/`](host/) | Wireshark `extcap` plugin + multi-channel aggregator (Python). Usage: [host/README.md](host/README.md) |
| [`hardware/`](hardware/) | Open hardware. **V1** = the fabricated board (JLCEDA Pro source + Gerbers/BOM/case); **V2** = WIP. See [hardware/README.md](hardware/README.md) — note the V1 known issues (single-board works; three-board needs rework). |
| [`tools/`](tools/) | Build / flash / RTT / extcap-install helper scripts |
| [`west.yml`](west.yml) | west manifest (pulls NCS from official Nordic GitHub) |

## Quick start

```bash
# 1) Firmware (from an empty workspace directory)
git clone https://github.com/BLEhound/BLEhound
west init -l BLEhound && west update && west zephyr-export
west build -b nrf54lm20dk/nrf54lm20a/cpuapp -s BLEhound/firmware \
  -- -DEXTRA_CONF_FILE=boards/blehound.conf \
     -DEXTRA_DTC_OVERLAY_FILE=boards/blehound.overlay
west flash            # or: BLEhound/tools/flash.sh

# 2) Host plugin
BLEhound/tools/install_extcap.sh      # copies the extcap into Wireshark
# then open Wireshark → interface "BLEhound Sniffer"
```

> **Windows** works too — the host tools are cross-platform. Wireshark on Windows
> needs the bundled `.bat` wrapper; see [host/README.md](host/README.md#windows).

Full walkthrough (single-board and three-board): **https://blehound.github.io**

## Licensing

- **Code** (firmware + host): [Apache-2.0](LICENSE)
- **Hardware** (schematic / PCB / Gerbers): [CERN-OHL-S-2.0](hardware/LICENSE)
- Third-party components (Zephyr, nRF Connect SDK, nrfxlib) keep their own licenses.

## Legal / ethical use

BLEhound captures radio traffic and includes legacy-pairing analysis tooling
(equivalent to `crackle`). Only use it on devices you own or are explicitly
authorized to test. Note that **LE Secure Connections (BLE 4.2+) cannot be passively
broken** — no sniffer can. See [host/security/README.md](host/security/README.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Issues and PRs welcome — the multi-channel
path in particular benefits from testing across more central/peripheral stacks.
