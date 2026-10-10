/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Direction of a data-channel packet within a connection event -- a pure function with no
 * hardware dependency, called by conn_follower and unit-tested on the host (test/host/test_dir_policy.c).
 *
 * Within a connection event the central and peripheral transmit strictly alternately, with one
 * T_IFS (150µs by default, negotiable in 6.0) between adjacent packets. So: the first packet of an
 * event = central -> peripheral; after that, every packet that follows one T_IFS later flips the
 * direction.
 *
 * The old rule was "everything after the first packet is peripheral -> central". When the central
 * sends two PDUs in one event (with DLE 251, a 498-byte ATT Write splits into 251+251, so every
 * event during OTA is C,P,C,P -- four packets) the second one was mislabelled, and the host
 * reassembled the ATT in the wrong direction, so half of the write commands could not be rebuilt.
 *
 * Dropped packets: when the air gap between two packets is clearly larger than one T_IFS, at least
 * one packet was missed in between. If the gap fits one shortest PDU (an empty packet) but not two,
 * exactly one was missed, so the direction is the same as the previous packet; a larger gap cannot
 * tell how many were missed, so it returns unknown, and later packets in the event derived from an
 * unknown stay unknown.
 */
#ifndef DIR_POLICY_H_
#define DIR_POLICY_H_

#include <stdint.h>

/** Direction values, matching HOST_DIR_* in host_iface.h */
#define DIR_UNKNOWN 0u
#define DIR_C2P     1u
#define DIR_P2C     2u

struct dir_tracker {
	uint8_t pkts;           /**< packets received in this event (saturating) */
	uint8_t last;           /**< previous packet's direction, DIR_* */
	uint32_t last_end_us;   /**< previous packet's air end (CRC tail) timestamp */
};

static inline void dir_tracker_reset(struct dir_tracker *t)
{
	t->pkts = 0;
	t->last = DIR_UNKNOWN;
	t->last_end_us = 0;
}

static inline uint8_t dir_flip(uint8_t d)
{
	if (d == DIR_C2P) {
		return DIR_P2C;
	}
	if (d == DIR_P2C) {
		return DIR_C2P;
	}
	return DIR_UNKNOWN;
}

/**
 * Another packet arrived in this event: return its direction and record the state.
 *
 * @param start_us        this packet's preamble start timestamp
 * @param end_us          this packet's CRC end timestamp
 * @param t_ifs_us        the link's effective inter-frame space (150 by default; the negotiated
 *                        value after a 6.0 negotiation)
 * @param min_pdu_air_us  air time of the shortest PDU (an empty packet, preamble -> CRC) on the
 *                        current PHY
 * @return DIR_C2P / DIR_P2C / DIR_UNKNOWN
 */
static inline uint8_t dir_tracker_on_packet(struct dir_tracker *t, uint32_t start_us,
					    uint32_t end_us, uint16_t t_ifs_us,
					    uint32_t min_pdu_air_us)
{
	uint8_t d;

	if (t->pkts == 0) {
		d = DIR_C2P;   /* the first packet of an event can only be the central's */
	} else {
		const uint32_t gap = start_us - t->last_end_us;   /* unsigned-wraparound safe */

		if (gap < 2u * (uint32_t)t_ifs_us) {
			/* Right after one T_IFS: the reply. Under two T_IFS cannot fit a dropped packet
			 * (even a 2M empty packet takes 44µs), so this bucket needs no extra tolerance. */
			d = dir_flip(t->last);
		} else if (gap < 3u * (uint32_t)t_ifs_us + 2u * min_pdu_air_us) {
			d = t->last;   /* exactly one was missed: same direction */
		} else {
			d = DIR_UNKNOWN;   /* two or more missed, cannot tell */
		}
	}
	if (t->pkts < 255u) {
		t->pkts++;
	}
	t->last = d;
	t->last_end_us = end_us;
	return d;
}

#endif /* DIR_POLICY_H_ */
