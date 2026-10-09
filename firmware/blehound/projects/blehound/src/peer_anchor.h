/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Anchor conversion for inter-board handoffs -- pure functions, no hardware dependency
 * (unit-tested in test/host/test_peer_anchor.c).
 *
 * The three chips keep independent time bases, so the anchor inside a handoff is an offset relative to
 * "the SYNC edge the discovering board pulled when it hit" (peer_handoff in peer_msg.h). All the receiver
 * has to do is recognise which of its own captured edges is that one:
 *   expected time = time the message arrived - send_age_us (the sender's delay from the edge to the SPI
 *                   transaction; the inter-board SPI itself takes at most a few hundred microseconds)
 * Pick the most recent captures' entry closest to the expected time, and it must fall within the tolerance
 * (edges are at least tens of ms apart; the 1 Hz heartbeat landing inside the window is negligible, and
 * even then the closest one wins).
 */

#ifndef PEER_ANCHOR_H_
#define PEER_ANCHOR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Maximum allowed deviation (us) between a captured edge and the expected time: SPI queueing + transfer +
 *  interrupt latency are far below this. */
#define PEER_ANCHOR_MATCH_TOL_US 3000u

/**
 * Find the tick in captures[0..n) closest to expect (32-bit wrapping difference); when the deviation is
 * <= tol, store it in *out and return true. Returns false for n == 0 or when nothing is within tolerance.
 */
static inline bool peer_anchor_match(const uint32_t *captures, size_t n, uint32_t expect,
				     uint32_t tol, uint32_t *out)
{
	bool found = false;
	uint32_t best_err = 0;
	uint32_t best = 0;

	for (size_t i = 0; i < n; i++) {
		const int32_t d = (int32_t)(captures[i] - expect);
		const uint32_t err = (uint32_t)(d < 0 ? -d : d);

		if (err <= tol && (!found || err < best_err)) {
			found = true;
			best_err = err;
			best = captures[i];
		}
	}
	if (found && out != NULL) {
		*out = best;
	}
	return found;
}

/** event0 anchor in the receiver's time base = the matched local capture + the sender's offset. */
static inline uint32_t peer_anchor_local(uint32_t matched_capture, int32_t anchor_offset_us)
{
	return matched_capture + (uint32_t)anchor_offset_us;
}

#endif /* PEER_ANCHOR_H_ */
