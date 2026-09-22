/*
 * Inter-board message serialization — the wire format the three boards use to notify / hand off to each other over point-to-point SPI (pure logic, no hardware dependency).
 *
 * Transport (SPIM/SPIS + REQ) lives in peer_link.*; this file only handles "struct ↔ byte stream",
 * so it can be rigorously round-trip / checksum / boundary tested on a PC.
 *
 * Wire format (all little-endian):
 *   [type:1][len:1][payload...][xor_checksum:1]
 *   len          = payload byte count (excluding type/len/checksum)
 *   xor_checksum = byte-wise XOR over [type][len][payload]
 *
 * Two message types (design §5.3):
 *   PEER_MSG_HIT      hit notification: I captured a CONNECT_IND for some AA (with the anchor tick in the common time base)
 *   PEER_MSG_HANDOFF  connection handoff: hand a connection's follow parameters to another board (strategy B)
 */

#ifndef PEER_MSG_H_
#define PEER_MSG_H_

#include <stddef.h>
#include <stdint.h>

enum peer_msg_type {
	PEER_MSG_HIT = 1,
	PEER_MSG_HANDOFF = 2,
};

/** Maximum byte count of one encoded message (largest payload = handoff's 30 + 3 header/trailer bytes) */
#define PEER_MSG_MAX 40

/** Hit notification: board_id captured a CONNECT_IND for AA=aa at anchor_sync_tick in the SYNC common time base */
struct peer_hit {
	uint8_t  board_id;
	uint32_t aa;
	uint32_t anchor_sync_tick;
};

/** Handoff: a connection's complete follow parameters (strategy B) */
struct peer_handoff {
	uint32_t aa;
	uint32_t crc_init;
	uint32_t ch_map_lo;        /* ChM low 32 bits */
	uint8_t  ch_map_hi;        /* ChM high 5 bits (37-bit channel map in total) */
	uint8_t  hop;
	uint8_t  sca;
	uint16_t interval;         /* connInterval, units of 1.25ms */
	uint16_t latency;
	uint16_t timeout;          /* supervisionTimeout, units of 10ms */
	uint16_t win_offset;       /* transmitWindowOffset, units of 1.25ms */
	uint8_t  win_size;         /* transmitWindowSize, units of 1.25ms */
	uint16_t event_counter;
	uint32_t anchor_sync_tick; /* event0 anchor's tick in the SYNC common time base */
};

/**
 * Encode. Writes into buf (capacity cap); returns the number of bytes written on success, or 0 if capacity is insufficient.
 */
size_t peer_msg_encode_hit(const struct peer_hit *m, uint8_t *buf, size_t cap);
size_t peer_msg_encode_handoff(const struct peer_handoff *m, uint8_t *buf, size_t cap);

/**
 * Decode. Identifies the type and validates len and checksum; on success fills the matching struct by type and returns the type
 * (PEER_MSG_HIT / PEER_MSG_HANDOFF), or 0 on failure. At least one of hit/ho must be non-NULL.
 */
int peer_msg_decode(const uint8_t *buf, size_t len,
		    struct peer_hit *hit, struct peer_handoff *ho);

#endif /* PEER_MSG_H_ */
