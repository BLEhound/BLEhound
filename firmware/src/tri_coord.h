/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Tri-board coordination layer — the platform-independent logic that ties P1/P2/P3 together (design §5).
 *
 * Each board guards one advertising channel (P1, decided by the straps); this layer is responsible for:
 *   - When a CONNECT_IND is captured on this board's channel, pulling a SYNC edge (P2, establishing the cross-board common time base)
 *     and broadcasting a hit notification over the inter-board SPI (P3);
 *   - Deciding "is this one mine to follow" via conn_follower's claim gate (strategy A/B/C, selected by Kconfig);
 *   - Handing the most recent SYNC edge tick (sync_epoch) to host_iface to fill the frame header, so the host can align the three merged streams.
 *
 * Whoever captures becomes the temporary master (the three boards are peers): there is no fixed follow master. The periodic time-base heartbeat is sent by board 0 (see periodic).
 *
 * Plan A (2026-09-27): besides following itself, the board that caught the CONNECT_IND hands the connection
 * parameters straight to the other two over the inter-board SPI (downstream via the master port write, upstream by
 * asking the peer to read via REQ); the anchor is expressed as "offset from this hit's SYNC edge" (peer_anchor.h).
 * The receiver converts it into its own time base and drops it into conn_follower's inject mailbox -- the same
 * take-over path as the host USB relay (HOST_CMD_FOLLOW); whichever arrives first takes over, the later one is
 * ignored as a duplicate.
 */

#ifndef TRI_COORD_H_
#define TRI_COORD_H_

#include <stdbool.h>
#include <stdint.h>

#include "radio_hal.h"

/** Coordination statistics (for observability) */
struct tri_coord_stats {
	uint32_t own_hits;         /**< CONNECT_INDs caught and handled by this board */
	uint32_t peer_hits;        /**< HIT messages received from peers (incl. the periodic link self-test, +2 per board per 10 s) */
	uint32_t handoffs_sent;    /**< connection handoffs sent (counted once per message, both links) */
	uint32_t handoffs_recv;    /**< connection handoffs received */
	uint32_t handoffs_injected;/**< handoffs whose anchor matched and were handed to the follower to take over */
	uint32_t handoffs_unmatched;/**< handoffs dropped because no matching SYNC edge was found among the recent captures */
	uint32_t rx_bad;           /**< inter-board frames received but undecodable (checksum/type/length) */
	uint32_t sync_emits;       /**< SYNC edges pulled by this board */
	uint32_t tx_dropped;       /**< messages dropped because the inter-board TX queue was full (should stay 0) */
};

/** Initialize: bring up the SYNC line and inter-board link, and register the claim gate. Call after radio_init/conn_follower_init. */
void tri_coord_init(uint8_t board_id);

/** Called for every received packet (receive-interrupt context). Detects a CONNECT_IND hit on this board's channel and coordinates. */
void tri_coord_on_packet(const struct radio_packet *pkt);

/** Periodic heartbeat (thread context, ~1Hz). board 0 uses it to keep emitting SYNC edges to calibrate the three boards' clocks (§3.3). */
void tri_coord_periodic(void);

/** The most recent SYNC edge's tick in this board's time base (fills the frame-header sync_epoch; 0 = none yet). */
uint32_t tri_coord_sync_epoch(void);

/** Claim-gate implementation: before starting to follow a captured CONNECT_IND, ask "is this one mine to follow" (per the Kconfig strategy). */
bool tri_coord_should_follow(uint32_t aa);

/** Read a snapshot of the coordination statistics. */
void tri_coord_get_stats(struct tri_coord_stats *out);

#endif /* TRI_COORD_H_ */
