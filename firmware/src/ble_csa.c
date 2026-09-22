/*
 * BLE channel selection algorithm implementation -- see ble_csa.h for the algorithm description.
 *
 * The implementation strictly follows Bluetooth Core Spec v5.2 Vol 6 Part B §4.5.8, and has been
 * cross-validated for equivalence against the Zephyr BLE controller's lll_chan.c on the host.
 */

#include "ble_csa.h"

/* Reverse the bit order of a byte (bit-hack, from Stanford bithacks) */
static uint8_t rev8(uint8_t b)
{
	return (uint8_t)((((uint32_t)b * 0x0802LU & 0x22110LU) |
			  ((uint32_t)b * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16);
}

/* CSA#2 permute: for a 16-bit value, bit-reverse the high and low bytes independently */
static uint16_t perm16(uint16_t i)
{
	return (uint16_t)((rev8((i >> 8) & 0xFF) << 8) | rev8(i & 0xFF));
}

/* CSA#2 MAM (one step of multiply-add-multiply): (a*17 + b) mod 2^16 */
static uint16_t mam(uint16_t a, uint16_t b)
{
	return (uint16_t)(((uint32_t)a * 17U + b) & 0xFFFF);
}

/* prn_s: permute + mam iterated 3 times */
static uint16_t prn_s(uint16_t counter, uint16_t chan_id)
{
	uint16_t v = counter ^ chan_id;

	for (int i = 0; i < 3; i++) {
		v = perm16(v);
		v = mam(v, chan_id);
	}

	return v;
}

/* prn_e = prn_s XOR chan_id */
static uint16_t prn_e(uint16_t counter, uint16_t chan_id)
{
	return prn_s(counter, chan_id) ^ chan_id;
}

/*
 * Map "the index-th available channel" back to the actual data channel number.
 * chan_map is a 5-byte little-endian bitmap; bit i indicates whether data channel i is available.
 */
static uint8_t remap(const uint8_t *chan_map, uint8_t index)
{
	uint8_t chan = 0;

	for (uint8_t byte = 0; byte < 5; byte++) {
		uint8_t bits = chan_map[byte];

		for (uint8_t b = 0; b < 8; b++) {
			if (bits & 0x01) {
				if (index == 0) {
					return chan;
				}
				index--;
			}
			chan++;
			bits >>= 1;
		}
	}

	/* Only reached if index exceeds the number of available channels; the caller guarantees this cannot happen */
	return 0;
}

uint16_t ble_csa_channel_id(uint32_t access_addr)
{
	const uint16_t aa_ls = access_addr & 0xFFFF;
	const uint16_t aa_ms = (access_addr >> 16) & 0xFFFF;

	return aa_ms ^ aa_ls;
}

uint8_t ble_csa_channel_count(const uint8_t *chan_map)
{
	uint8_t count = 0;

	for (uint8_t byte = 0; byte < 5; byte++) {
		uint8_t bits = chan_map[byte];

		while (bits) {
			count += bits & 0x01;
			bits >>= 1;
		}
	}

	return count;
}

uint8_t ble_csa1_next(uint8_t *last_unmapped, uint8_t hop, uint16_t latency,
		      const uint8_t *chan_map, uint8_t chan_count)
{
	/* Unmapped channel: add hop*(1+latency) to the last value, modulo 37 */
	const uint8_t unmapped =
		(uint8_t)((*last_unmapped + hop * (1 + latency)) % BLE_DATA_CHANNELS);

	*last_unmapped = unmapped;

	/* If the channel is available, use it directly; otherwise remap to "the (unmapped % count)-th available channel" */
	if (chan_map[unmapped >> 3] & (1 << (unmapped & 0x07))) {
		return unmapped;
	}

	return remap(chan_map, unmapped % chan_count);
}

uint8_t ble_csa2_next(uint16_t counter, uint16_t chan_id,
		      const uint8_t *chan_map, uint8_t chan_count)
{
	const uint16_t pe = prn_e(counter, chan_id);
	const uint8_t unmapped = pe % BLE_DATA_CHANNELS;

	if (chan_map[unmapped >> 3] & (1 << (unmapped & 0x07))) {
		return unmapped;
	}

	/* CSA#2 remap index: (count * prn_e) >> 16 */
	const uint8_t index = (uint8_t)(((uint32_t)chan_count * pe) >> 16);

	return remap(chan_map, index);
}

/* ---------------- CSA#2 ISO variant (multi-subevent hopping within a BIS / CIS event) ------- */

/*
 * Inverse of remap(): given an "available" actual channel number, find its index among the
 * available channels. Subevent recursion adds in the "available channel index" space, so if the
 * first subevent lands directly on an available channel (without going through remapping), it must
 * still be converted back to an index first, otherwise the subsequent subevents cannot follow on.
 * Corresponds to Zephyr lll_chan.c: chan_sel_remap_index().
 */
static uint8_t remap_index_of(const uint8_t *chan_map, uint8_t chan)
{
	uint8_t index = 0;

	for (uint8_t byte = 0; byte < 5; byte++) {
		uint8_t bits = chan_map[byte];

		for (uint8_t b = 0; b < 8; b++) {
			if (chan == 0) {
				return index;
			}
			chan--;

			if (bits & 0x01) {
				index++;
			}
			bits >>= 1;
		}
	}

	return 0;
}

/* Subevent pseudo-random number: run one more permute+MAM iteration on prn_lu, output lu XOR chan_id.
 * BT Core Spec §4.5.8.3.5 / Zephyr chan_prn_subevent_se(). */
static uint16_t prn_subevent_se(uint16_t chan_id, uint16_t *prn_lu)
{
	uint16_t lu = *prn_lu;

	lu = perm16(lu);
	lu = mam(lu, chan_id);
	*prn_lu = lu;

	return (uint16_t)(lu ^ chan_id);
}

/* Step size d in the subevent mapping, depends only on the number of available channels N.
 * d = max(1, max(min(3, N-5), min(11, (N-10)/2)))
 * BT Core Spec §4.5.8.3.6 / Zephyr chan_d(). */
static uint8_t chan_step_d(uint8_t n)
{
	const uint8_t x = (n > 5) ? (uint8_t)(n - 5) : 0u;
	const uint8_t y = (n > 10) ? (uint8_t)((n - 10) >> 1) : 0u;
	const uint8_t a = (x < 3) ? x : 3u;
	const uint8_t b = (y < 11) ? y : 11u;
	const uint8_t m = (a > b) ? a : b;

	return (m > 1) ? m : 1u;
}

uint8_t ble_csa2_iso_event(uint16_t counter, uint16_t chan_id,
			   const uint8_t *chan_map, uint8_t chan_count,
			   uint16_t *prn_lu, uint16_t *remap_idx)
{
	/* Note: the recursion state stores prn_s (the value **before** XOR with chan_id) */
	*prn_lu = prn_s(counter, chan_id);

	const uint16_t pe = (uint16_t)(*prn_lu ^ chan_id);
	const uint8_t unmapped = pe % BLE_DATA_CHANNELS;

	if (chan_map[unmapped >> 3] & (1 << (unmapped & 0x07))) {
		*remap_idx = remap_index_of(chan_map, unmapped);
		return unmapped;
	}

	*remap_idx = (uint16_t)(((uint32_t)chan_count * pe) >> 16);

	return remap(chan_map, (uint8_t)*remap_idx);
}

uint8_t ble_csa2_iso_subevent(uint16_t chan_id, const uint8_t *chan_map,
			      uint8_t chan_count, uint16_t *prn_lu,
			      uint16_t *remap_idx)
{
	const uint16_t se = prn_subevent_se(chan_id, prn_lu);
	const uint8_t d = chan_step_d(chan_count);
	/* Natural number (N + 1 - 2d), clamped to 0 on underflow */
	const uint8_t x = ((chan_count + 1) > (d << 1))
				  ? (uint8_t)((chan_count + 1) - (d << 1))
				  : 0u;

	*remap_idx = (uint16_t)((((((uint32_t)se * x) >> 16)) + d + *remap_idx) %
				chan_count);

	return remap(chan_map, (uint8_t)*remap_idx);
}
