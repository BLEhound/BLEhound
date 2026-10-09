/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Inter-board link HAL -- pairwise point-to-point SPI + REQ (design 4.2 / 5.3, P3 coordination transport).
 *
 * Topology: the three chips are connected pairwise (links AB/BC/CA); each chip has one resident **master
 * port** (SPIM00 pins, driving its downstream slave) and one resident **slave port** (SPIS, selected by its
 * upstream master). One REQ line per link: the slave side pulls REQ when it wants to talk and the master
 * side, on the interrupt, starts one read transaction to fetch the message -- so every link has exactly one
 * master and the firmware needs no arbitration.
 *
 * This HAL wraps "how a message physically gets from A to B"; message encoding lives in peer_msg.* and what
 * to do with a message (hit broadcast / connection handoff) in tri_coord.*.
 *
 * Implementation (peer_link.c):
 *   - Downstream master port: **software (bit-banged) SPI** on GPIOs (DT aliases tri-spim-sck/mosi/miso +
 *     tri-peer-cs, mode 0, MSB first, ~400 kHz). SPIM00 is not used: on the nRF54LM20A it only outputs SCK
 *     on the dedicated clock pin P2.01, the V1 board wires SCK to P2.02 (measured 2026-09-27: never toggles),
 *     and P2.01 is the old SYNC net shared by all three chips.
 *     (Another, now irrelevant pitfall: SPIM00 prescaler is 4..126, so 1 MHz is rejected by nrfx with -22.)
 *   - Upstream slave port: nrfx_spis driven directly (instance spi21, pins from DT aliases tri-spis-*), kept
 *     armed with a 40-byte RX + 40-byte TX buffer and re-armed inside the interrupt after every transaction;
 *     messages pushed by the master arrive through on_recv (interrupt context). The TX buffer is the message
 *     "waiting for the upstream master to read it".
 *   - REQ: to send to the upstream master, write the message into the SPIS TX buffer and pulse REQ_out;
 *     when the downstream slave pulls this chip's REQ_in, the interrupt submits a system work item and the
 *     thread reads 40 bytes over the master port (on_recv then runs in thread context).
 *   - Without the inter-board DT aliases (single board / DK): init logs and disables, sends return -ENOTSUP,
 *     everything else works as on a single board.
 */

#ifndef PEER_LINK_H_
#define PEER_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Called when a peer message arrives. From the slave port it runs in the SPIS interrupt, from a REQ read in
 *  the system work queue thread; keep it short either way. */
typedef void (*peer_recv_cb_t)(const uint8_t *msg, size_t len);

/**
 * Initialise the inter-board link: master port GPIOs, slave port SPIS, REQ GPIOs (in = interrupt, out = output).
 * Returns 0 but stays disabled when the tri-peer aliases are not defined.
 * @return 0 on success or when disabled; negative errno when the master port is unusable
 */
int peer_link_init(peer_recv_cb_t on_recv);

/** As the downstream master, send one message (<= PEER_MSG_MAX) to the downstream slave. **Thread context**
 *  (synchronous bit-banged transaction). -ENOTSUP when disabled. */
int peer_link_send(const uint8_t *msg, size_t len);

/** As the upstream slave, load the message into the SPIS TX buffer and pulse REQ so the upstream master reads
 *  it. Any context. -ENOTSUP without a slave port. */
int peer_link_send_upstream(const uint8_t *msg, size_t len);

/** Link diagnostics counters */
struct peer_link_stats {
	uint32_t spim_tx;        /**< master port write transactions */
	uint32_t spim_tx_err;    /**< (reserved) master port write failures */
	uint32_t spim_rx;        /**< REQ-triggered master port read transactions */
	uint32_t spis_rx;        /**< messages received and reported by the slave port */
	uint32_t spis_xfer;      /**< slave port transactions in total (including the upstream master reading us) */
	uint32_t spis_arm_err;   /**< slave port buffer re-arm failures (should stay 0) */
	uint32_t req_out;        /**< REQ pulses emitted by this chip */
	int32_t  last_err;       /**< most recent slave port error code */
	bool     spis_ready;     /**< slave port armed */
};

void peer_link_get_stats(struct peer_link_stats *out);

#endif /* PEER_LINK_H_ */
