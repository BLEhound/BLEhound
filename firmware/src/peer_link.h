/*
 * Inter-board link HAL — pairwise point-to-point SPI + REQ (design §4.2 / §5.3, P3 coordinated transfer).
 *
 * Topology: the three boards are pairwise interconnected (links AB/BC/CA), each with one always-on **master port** (SPIM, driving its outgoing-link slave) +
 * one always-on **slave port** (SPIS, selected by the incoming-link master). One REQ per link: the slave end pulls REQ when it wants to talk,
 * and the master starts a transaction to read the message out once it gets the interrupt — so each link always has exactly one master and the firmware needs zero arbitration.
 *
 * This HAL wraps "how to physically move a message from A to B on the hardware"; encoding/decoding of message content lives in peer_msg.*,
 * and what to do with a message (hit dedup / handoff) lives in tri_coord.*.
 *
 * Implementation status (see peer_link.c):
 *   - **Outgoing-link master-port send + REQ-triggered master-port read**: implemented with the Zephyr SPI master API (via DT alias
 *     tri-peer-spi pointing to the concrete SPIM; REQ uses GPIO). On the DK the alias points to spi00 for compile/link verification.
 *   - **Incoming-link slave-port receive (SPIS)**: needs nRF SPIS (nrfx) and a second board acting as master to verify —
 *     this is **on-board bring-up**; the recv callback path is ready, and the SPIS load point is marked `⚠️ verify on hardware`.
 *   - When the inter-board alias is undefined (single board): init logs a warning and disables, all sends return -ENOTSUP, and the single board runs as usual.
 */

#ifndef PEER_LINK_H_
#define PEER_LINK_H_

#include <stddef.h>
#include <stdint.h>

/** Callback when a peer message arrives (REQ-triggered master-port read / incoming-link slave-port complete). Interrupt / system-work context, keep it short. */
typedef void (*peer_recv_cb_t)(const uint8_t *msg, size_t len);

/**
 * Initialize the inter-board link: bring up the master-port SPI and the REQ GPIOs (in = interrupt, out = output).
 * Returns 0 but stays disabled when the tri-peer DT alias is undefined.
 * @return 0 on success or when disabled; negative for errno
 */
int peer_link_init(peer_recv_cb_t on_recv);

/** As the outgoing-link master port, send one message to the outgoing-link slave (≤ PEER_MSG_MAX). Returns -ENOTSUP when disabled. */
int peer_link_send(const uint8_t *msg, size_t len);

/** As the incoming-link slave, pull REQ to ask the incoming-link master to read this board (paired with the SPIS load, on-board bring-up). */
void peer_link_request(void);

#endif /* PEER_LINK_H_ */
