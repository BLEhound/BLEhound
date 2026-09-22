/*
 * Inter-board message serialization implementation — see peer_msg.h. Pure C, no Zephyr/hardware dependency (unit-testable on host).
 */

#include "peer_msg.h"

/* ---- Little-endian read/write helpers (no dependency on Zephyr sys_put/get, so host tests also compile) ---- */

static void put_u8(uint8_t **p, uint8_t v)
{
	*(*p)++ = v;
}

static void put_u16(uint8_t **p, uint16_t v)
{
	*(*p)++ = (uint8_t)(v & 0xFF);
	*(*p)++ = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t **p, uint32_t v)
{
	*(*p)++ = (uint8_t)(v & 0xFF);
	*(*p)++ = (uint8_t)((v >> 8) & 0xFF);
	*(*p)++ = (uint8_t)((v >> 16) & 0xFF);
	*(*p)++ = (uint8_t)((v >> 24) & 0xFF);
}

static uint8_t get_u8(const uint8_t **p)
{
	return *(*p)++;
}

static uint16_t get_u16(const uint8_t **p)
{
	uint16_t v = (uint16_t)((*p)[0]) | (uint16_t)((*p)[1] << 8);
	*p += 2;
	return v;
}

static uint32_t get_u32(const uint8_t **p)
{
	uint32_t v = (uint32_t)((*p)[0]) | ((uint32_t)(*p)[1] << 8) |
		     ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
	*p += 4;
	return v;
}

static uint8_t xor_checksum(const uint8_t *buf, size_t n)
{
	uint8_t c = 0;

	for (size_t i = 0; i < n; i++) {
		c ^= buf[i];
	}
	return c;
}

/* payload length (excluding type/len/checksum) */
#define HIT_PAYLOAD_LEN     9    /* board_id(1) + aa(4) + anchor(4) */
#define HANDOFF_PAYLOAD_LEN 30   /* sum of the peer_handoff fields */

/* Finalize: write the checksum and return the total frame length */
static size_t finalize(uint8_t *buf, size_t payload_len)
{
	const size_t frame_len = 2 + payload_len;   /* type + len + payload */

	buf[frame_len] = xor_checksum(buf, frame_len);
	return frame_len + 1;                        /* + checksum */
}

size_t peer_msg_encode_hit(const struct peer_hit *m, uint8_t *buf, size_t cap)
{
	const size_t need = 2 + HIT_PAYLOAD_LEN + 1;

	if (m == NULL || buf == NULL || cap < need) {
		return 0;
	}

	uint8_t *p = buf;

	put_u8(&p, PEER_MSG_HIT);
	put_u8(&p, HIT_PAYLOAD_LEN);
	put_u8(&p, m->board_id);
	put_u32(&p, m->aa);
	put_u32(&p, m->anchor_sync_tick);

	return finalize(buf, HIT_PAYLOAD_LEN);
}

size_t peer_msg_encode_handoff(const struct peer_handoff *m, uint8_t *buf, size_t cap)
{
	const size_t need = 2 + HANDOFF_PAYLOAD_LEN + 1;

	if (m == NULL || buf == NULL || cap < need) {
		return 0;
	}

	uint8_t *p = buf;

	put_u8(&p, PEER_MSG_HANDOFF);
	put_u8(&p, HANDOFF_PAYLOAD_LEN);
	put_u32(&p, m->aa);
	put_u32(&p, m->crc_init);
	put_u32(&p, m->ch_map_lo);
	put_u8(&p, m->ch_map_hi);
	put_u8(&p, m->hop);
	put_u8(&p, m->sca);
	put_u16(&p, m->interval);
	put_u16(&p, m->latency);
	put_u16(&p, m->timeout);
	put_u16(&p, m->win_offset);
	put_u8(&p, m->win_size);
	put_u16(&p, m->event_counter);
	put_u32(&p, m->anchor_sync_tick);

	return finalize(buf, HANDOFF_PAYLOAD_LEN);
}

int peer_msg_decode(const uint8_t *buf, size_t len,
		    struct peer_hit *hit, struct peer_handoff *ho)
{
	if (buf == NULL || len < 3) {
		return 0;   /* at least type + len + checksum */
	}

	const uint8_t type = buf[0];
	const uint8_t payload_len = buf[1];
	const size_t frame_len = 2 + (size_t)payload_len;

	/* the total frame length must equal 2 + payload + 1 (checksum) and stay in bounds */
	if (len < frame_len + 1) {
		return 0;
	}
	if (buf[frame_len] != xor_checksum(buf, frame_len)) {
		return 0;
	}

	const uint8_t *p = &buf[2];

	switch (type) {
	case PEER_MSG_HIT:
		if (payload_len != HIT_PAYLOAD_LEN || hit == NULL) {
			return 0;
		}
		hit->board_id = get_u8(&p);
		hit->aa = get_u32(&p);
		hit->anchor_sync_tick = get_u32(&p);
		return PEER_MSG_HIT;

	case PEER_MSG_HANDOFF:
		if (payload_len != HANDOFF_PAYLOAD_LEN || ho == NULL) {
			return 0;
		}
		ho->aa = get_u32(&p);
		ho->crc_init = get_u32(&p);
		ho->ch_map_lo = get_u32(&p);
		ho->ch_map_hi = get_u8(&p);
		ho->hop = get_u8(&p);
		ho->sca = get_u8(&p);
		ho->interval = get_u16(&p);
		ho->latency = get_u16(&p);
		ho->timeout = get_u16(&p);
		ho->win_offset = get_u16(&p);
		ho->win_size = get_u8(&p);
		ho->event_counter = get_u16(&p);
		ho->anchor_sync_tick = get_u32(&p);
		return PEER_MSG_HANDOFF;

	default:
		return 0;
	}
}
