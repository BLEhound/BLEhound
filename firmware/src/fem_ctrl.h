/*
 * nRF21540 FEM control -- pure GPIO mode (no SPI). One FEM per SoC (hardware README "nRF21540 FEM").
 *
 * Why GPIO only, no register writes:
 *   The nRF21540 works at factory defaults right after power-up: PA gain is selected by the MODE pin
 *   between the two factory presets POUTA (+20 dBm) / POUTB (+10 dBm), the PA/LNA switching is all
 *   done via TX_EN/RX_EN, and driving PDN high enters the PG (powered-up) state -- not a single
 *   register write needed. SPI is only an "optional override" (fine-tuning TX gain / reading the ID);
 *   the sniffer only receives and never transmits, so it is not needed.
 *
 * ⚠️ Hard rule (early nRF54LM20A samples):
 *   On some early nRF54LM20A samples, QFN52 pin 30 (P0.09) is tied to VDD inside the package and is
 *   not a GPIO, yet on the full board P0.09 happens to connect to the FEM's MISO. The FEM's MISO is
 *   only driven when CSN is low and is high-Z when CSN is high, so as long as **CSN always outputs
 *   high, SCK/MOSI are not used for SPI, and MISO (P0.09) is never configured**, such a sample is
 *   indistinguishable from the production part and there is no current path on the line. Whoever
 *   changes this must not pull CSN low or initialize anything on P0.09.
 *
 * State machine (throughout): PD --PDN=1--> PG --RX_EN=1--> RX (LNA always on). The sniffer receives
 * the whole time, and the LNA is not switched on/off with the radio's start/stop.
 * The PA goes through fem_ctrl_set_tx(): TX_EN/RX_EN switch mutually exclusively, waiting for the
 * nRF21540 PS Table 6 settling times.
 * The firmware currently does not transmit; this interface is reserved (injection / DTM).
 *
 * DT: the tri_fem node + fem-* aliases in the overlay. A missing alias is treated as no FEM (e.g. the
 * nRF52840 DK); init returns 0 but the feature is disabled -- the same convention as sync_line / peer_link.
 */

#ifndef FEM_CTRL_H_
#define FEM_CTRL_H_

#include <stdbool.h>

/**
 * Power up the FEM and keep the LNA always on: CSN high → each control pin to a defined state →
 * PDN high (wait PD→PG) → RX_EN high (wait PG→RX).
 * Must be called **before** the radio starts receiving: the nRF21540 has no bypass path, so the RF path is broken when PDN=0.
 * @return 0 on success or no FEM (disabled); a negative errno (GPIO not ready / configuration failed)
 */
int fem_ctrl_init(void);

/**
 * PA switch (GPIO controlled). on=true: RX_EN low → TX_EN high, wait for the PA to settle; on=false: return to LNA always on.
 * Blocks for the settling time (tens of µs); a no-op when there is no FEM.
 */
void fem_ctrl_set_tx(bool on);

#endif /* FEM_CTRL_H_ */
