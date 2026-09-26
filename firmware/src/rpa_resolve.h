/*
 * Resolvable private address (RPA) resolution -- pure functions + a small cache, no hardware dependency,
 * called from main's RX interrupt / capture thread and unit-tested on the host (test/host/test_rpa.c in
 * the internal repo).
 *
 * Background: privacy-enabled devices such as the ring change their RPA periodically (or on every
 * reconnect). With MAC-only filtering the firmware keeps staring at the old address after a change and
 * every advertisement / CONNECT_IND from the new one is blocked. Given the device's IRK, ah() from
 * BT Core Spec Vol 3, Part H 2.2.2 decides "is this RPA that device":
 *     hash = e(IRK, padding || prand) mod 2^24,   RPA = prand (24 bits, top 2 bits = 01) || hash
 *
 * Division of work:
 *   - the RX interrupt only consults the cache (a few memcmp); an RPA never seen before is recorded as
 *     "pending" and treated as unrelated;
 *   - the capture thread runs AES with the IRK on the pending entries, marks them match/no-match, and on a
 *     match makes that address the current target MAC.
 * So AES never runs in the interrupt, and a new RPA loses at most its first one or two advertisements.
 *
 * Byte-order convention (verified on real hardware, 2026-09-26): the IRK is passed in **over-the-air /
 * SMP Identity Information order (LSO first)**, matching the device log line "IRK wire/LSO-first"; the AES
 * key is those 16 bytes reversed. Addresses are over-the-air little-endian.
 */
#ifndef RPA_RESOLVE_H_
#define RPA_RESOLVE_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ble_aes128.h"

#define RPA_CACHE_SIZE 16

enum rpa_state {
	RPA_UNKNOWN = 0,   /**< empty slot */
	RPA_PENDING,       /**< seen by the interrupt, not computed yet */
	RPA_MATCH,         /**< resolves with the current IRK */
	RPA_NOMATCH,       /**< does not resolve (someone else's RPA) */
};

struct rpa_cache_entry {
	uint8_t addr[6];
	volatile uint8_t state;   /**< enum rpa_state */
};

struct rpa_cache {
	struct rpa_cache_entry e[RPA_CACHE_SIZE];
	uint8_t next;             /**< round-robin replacement cursor */
};

/** Whether a random address is a resolvable private address: top 2 bits = 0b01. addr is over-the-air little-endian (addr[5] is the most significant byte). */
static inline bool rpa_is_resolvable(const uint8_t addr_le[6])
{
	return (addr_le[5] & 0xC0) == 0x40;
}

/**
 * ah() check: does the IRK resolve this RPA?
 * @param irk_le  16 bytes, over-the-air / SMP order (LSO first)
 * @param addr_le 6 bytes, over-the-air little-endian
 */
static inline bool rpa_matches_irk(const uint8_t irk_le[16], const uint8_t addr_le[6])
{
	uint8_t key[16];
	uint8_t blk[16] = {0};
	uint8_t out[16];

	if (!rpa_is_resolvable(addr_le)) {
		return false;
	}
	for (int i = 0; i < 16; i++) {
		key[i] = irk_le[15 - i];
	}
	/* r' = padding (13 zero bytes) || prand (MSO first) */
	blk[13] = addr_le[5];
	blk[14] = addr_le[4];
	blk[15] = addr_le[3];
	ble_aes128_encrypt(key, blk, out);
	return out[13] == addr_le[2] && out[14] == addr_le[1] && out[15] == addr_le[0];
}

static inline void rpa_cache_reset(struct rpa_cache *c)
{
	memset(c, 0, sizeof(*c));
}

/**
 * Interrupt side: look up the resolution result of this RPA; if never seen, record it as pending and
 * return RPA_PENDING. memcmp only, no AES.
 */
static inline enum rpa_state rpa_cache_lookup(struct rpa_cache *c, const uint8_t addr_le[6])
{
	for (int i = 0; i < RPA_CACHE_SIZE; i++) {
		if (c->e[i].state != RPA_UNKNOWN && memcmp(c->e[i].addr, addr_le, 6) == 0) {
			return (enum rpa_state)c->e[i].state;
		}
	}
	struct rpa_cache_entry *e = &c->e[c->next];

	c->next = (uint8_t)((c->next + 1) % RPA_CACHE_SIZE);
	e->state = RPA_UNKNOWN;          /* invalidate first, then fill the address, then set the state: the thread side at worst sees it one round late */
	memcpy(e->addr, addr_le, 6);
	e->state = RPA_PENDING;
	return RPA_PENDING;
}

/**
 * Thread side: resolve one pending entry.
 * @param addr_out the entry's address (over-the-air little-endian)
 * @param matched  whether it resolves
 * @return true = one entry was processed (call in a loop until it returns false)
 */
static inline bool rpa_cache_resolve_one(struct rpa_cache *c, const uint8_t irk_le[16],
					 uint8_t addr_out[6], bool *matched)
{
	for (int i = 0; i < RPA_CACHE_SIZE; i++) {
		struct rpa_cache_entry *e = &c->e[i];

		if (e->state != RPA_PENDING) {
			continue;
		}
		memcpy(addr_out, e->addr, 6);
		*matched = rpa_matches_irk(irk_le, addr_out);
		/* The interrupt may have replaced this slot with another address while we computed: only write the result back if it is still the same address */
		if (e->state == RPA_PENDING && memcmp(e->addr, addr_out, 6) == 0) {
			e->state = *matched ? RPA_MATCH : RPA_NOMATCH;
		}
		return true;
	}
	return false;
}

#endif /* RPA_RESOLVE_H_ */
