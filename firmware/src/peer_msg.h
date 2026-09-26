/*
 * Inter-board message serialisation -- the wire format the three boards use to report hits / hand off
 * connections over the pairwise SPI links (pure logic, no hardware dependency).
 *
 * Transport (SPIM/SPIS + REQ) lives in peer_link.*; this file only does "struct <-> byte stream", so it can be
 * tested strictly on a PC (round trip / checksum / bounds).
 *
 * Wire format (all little-endian):
 *   [type:1][len:1][payload...][xor_checksum:1]
 *   len          = payload bytes (excluding type/len/checksum)
 *   xor_checksum = XOR of every byte of [type][len][payload]
 *
 * Two messages (design 5.3):
 *   PEER_MSG_HIT      hit notification: I caught a CONNECT_IND for some AA (with the anchor tick in the common time base)
 *   PEER_MSG_HANDOFF  connection handoff: hand the following parameters of one connection to the peers
 */

#ifndef PEER_MSG_H_
#define PEER_MSG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum peer_msg_type {
	PEER_MSG_HIT = 1,
	PEER_MSG_HANDOFF = 2,
};

/** Maximum encoded message size (largest payload = the handoff's 30 + 3 bytes of framing) */
#define PEER_MSG_MAX 40

/** Hit notification: board_id caught a CONNECT_IND for AA=aa at anchor_sync_tick in the SYNC common time base */
struct peer_hit {
	uint8_t  board_id;
	uint32_t aa;
	uint32_t anchor_sync_tick;
};

/**
 * Handoff: the complete following parameters of one connection (plan A: the board that caught the CONNECT_IND
 * pushes them straight to the other two).
 *
 * The anchor is not an absolute tick (the three time bases are independent) but relative to "the SYNC edge
 * the discovering board pulled on this hit":
 *   anchor_offset_us = event0 anchor (sender time base) - the edge time the sender recorded
 *   send_age_us      = how long after that edge the SPI transaction actually went out (filled by the sending
 *                      thread right before sending, see set_age)
 * The receiver looks for the captured edge closest to (arrival time - send_age_us) in its own time base and
 * adds anchor_offset_us back: that is the event0 anchor in its own time base (error = capture interrupt
 * jitter, microseconds).
 */
struct peer_handoff {
	uint32_t aa;
	uint32_t crc_init;
	uint32_t ch_map_lo;        /* ChM low 32 bits */
	uint8_t  ch_map_hi;        /* ChM high 5 bits (37-bit channel map in total) */
	uint8_t  hop;
	uint8_t  csa2;             /* 1 = CSA#2 (ChSel bit of the CONNECT_IND) */
	uint16_t interval;         /* connInterval, unit 1.25 ms */
	uint16_t latency;
	uint16_t timeout;          /* supervisionTimeout, unit 10 ms */
	uint8_t  win_size;         /* transmitWindowSize, unit 1.25 ms */
	int32_t  anchor_offset_us; /* event0 anchor relative to the hit SYNC edge (sender time base) */
	uint32_t send_age_us;      /* time from the hit SYNC edge to the send (sender time base) */
};

/**
 * Encode. Writes into buf (capacity cap); returns the number of bytes written, 0 when it does not fit.
 */
size_t peer_msg_encode_hit(const struct peer_hit *m, uint8_t *buf, size_t cap);
size_t peer_msg_encode_handoff(const struct peer_handoff *m, uint8_t *buf, size_t cap);

/**
 * Decode. Recognises the type, validates len and checksum; on success fills the matching struct and returns
 * the type (PEER_MSG_HIT / PEER_MSG_HANDOFF), 0 on failure. At least one of hit/ho must be non-NULL.
 */
int peer_msg_decode(const uint8_t *buf, size_t len,
		    struct peer_hit *hit, struct peer_handoff *ho);

/**
 * Rewrite send_age_us of an already encoded HANDOFF in place and recompute the checksum (the sending thread
 * calls this right before the transaction). buf/len must be the complete output of encode_handoff; returns
 * false when it is not a HANDOFF or the length is wrong.
 */
bool peer_msg_handoff_set_age(uint8_t *buf, size_t len, uint32_t send_age_us);

#endif /* PEER_MSG_H_ */
