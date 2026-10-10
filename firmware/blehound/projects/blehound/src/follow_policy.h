/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Connection follow policy -- pure functions with no hardware dependency, called by conn_follower
 * and unit-tested on the host (test/host/test_follow_policy.c in the internal repo).
 *
 * Principle: until a target is set, only observe advertising and follow no connection; once a
 * target is set, follow only that target's connections.
 *   - Single-target mode (default): a target MAC must be set, the CONNECT_IND's AdvA must equal
 *     the target, and no other connection may be followed right now. No target = pure scanning,
 *     so the host can list the advertisers for the user to pick from first.
 *   - Multi-target evaluation mode: follow everything (only the target if one is set), up to
 *     CONN_FOLLOW_MAX_SLOTS connections.
 * A FOLLOW relayed by the host (tri-board joint follow) carries no AdvA to compare; the host has
 * already applied the target filter, so only the mode and whether a target is set are checked.
 */
#ifndef FOLLOW_POLICY_H_
#define FOLLOW_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

/** CONNECT_IND / AUX_CONNECT_REQ captured: start following? */
static inline bool follow_policy_allows(bool single_target, bool target_active,
					bool adva_matches_target, uint8_t active_connections)
{
	if (target_active && !adva_matches_target) {
		return false;   /* In any mode, once a target is set only the target is followed */
	}
	if (!single_target) {
		return true;    /* Evaluation mode: take everything */
	}
	return target_active && active_connections == 0;
}

/** FOLLOW relayed by the host received: take it over? */
static inline bool follow_policy_allows_inject(bool single_target, bool target_active,
					       uint8_t active_connections)
{
	if (!single_target) {
		return true;
	}
	return target_active && active_connections == 0;
}

/**
 * On catching a CONNECT_IND ourselves: should we release the existing ACL slot (preempt) before
 * following? In single-target mode the target's own new CONNECT_IND is proof the old connection is
 * dead -- an encrypted link never shows LL_TERMINATE_IND, so the stale slot would keep receiving
 * nothing until supervision timeout, during which follow_policy_allows's active_connections==0
 * would block the target's reconnection (2026-10-10: a 1s-advertising peripheral's reconnect missed
 * its CONNECT_IND).
 */
static inline bool follow_policy_preempts_on_connect(bool single_target, bool adva_matches_target)
{
	return single_target && adva_matches_target;
}

/**
 * On a relayed/inter-board inject: release one existing ACL slot? An inject has no AdvA to compare,
 * so only release a stale slot (miss_count>0); a connection still receiving normally is untouched.
 */
static inline bool follow_policy_preempts_on_inject(bool single_target, uint16_t slot_miss_count)
{
	return single_target && slot_miss_count > 0;
}

/**
 * While scanning, on the target's own **connectable** advertising (ADV_IND / ADV_DIRECT_IND):
 * release one existing ACL slot? A peripheral in a connection does not send connectable advertising,
 * so seeing it means the old connection is dead (reset after OTA, or the advertising burst before a
 * phone reconnects). Only release a stale slot (miss_count>0); a connection receiving normally is
 * untouched.
 */
static inline bool follow_policy_preempts_on_target_adv(bool single_target, bool adva_matches_target,
							uint16_t slot_miss_count)
{
	return single_target && adva_matches_target && slot_miss_count > 0;
}

/**
 * In the single-target relock state, should we yield one extra interval back to the guard channel
 * after this event? Relock widens the window exponentially up to nearly one interval, so there is no
 * longer a >=4ms gap between events and all three boards are off the advertising channels until the
 * old connection's supervision timeout. Alternating "re-search one event / yield one event" halves
 * the re-search rate, but half the time a guarding board can see the target's advertising and
 * CONNECT_IND before it reconnects; combined with preempts_on_target_adv (release the stale slot and
 * return to full-time guarding as soon as the target's advertising is seen). Alternate on the parity
 * of (miss_count - relock_after), so the event just after entering relock yields first.
 */
static inline bool follow_policy_relock_yields_scan(bool single_target, uint16_t miss_count,
						    uint16_t relock_after)
{
	if (!single_target || miss_count < relock_after) {
		return false;
	}
	return ((miss_count - relock_after) & 1u) == 0u;
}

/**
 * Establishment-failure test (Core Vol 6 Part B 4.5.2): if no packet from the peer is received
 * within 6 connection intervals after the CONNECT_IND, the central stops transmitting and the
 * peripheral goes back to advertising, and both treat establishment as failed. What the follower
 * sees: the central's packets are heard (an empty packet or the first control PDU, with SN/NESN
 * unchanged), the peripheral never answers, then the central stops too. The stale slot must be
 * released back to guarding immediately -- the loss threshold derived from the supervision timeout
 * (9.6s is 160 events) is far too long, all three boards would chase this dead connection, nobody
 * would be guarding for the stack's CONNECT_IND retry 0.5s later, and after that the peripheral
 * enters the connection and stops advertising, so the whole capture goes blank (2026-10-10
 * error.pcapng: the host connected at 7.6s, the sniffer caught nothing after that failed attempt).
 * never_heard: neither side was ever heard (anchors all off, or the handoff was late) and the window
 * is already at a full interval yet still missing -- give up likewise; a normal connection always
 * hears at least one side within the first few events.
 */
#define FOLLOW_ESTABLISH_GIVE_UP_MISS 2u    /* central heard, peripheral never answered: give up after this many missed events */
#define FOLLOW_NEVER_HEARD_MISS       12u   /* neither side ever heard: give up after this many (the widest relock bucket is done) */

static inline bool follow_policy_never_established(bool central_seen, bool peripheral_seen,
						   uint16_t miss_count)
{
	if (peripheral_seen) {
		return false;   /* the peripheral answered = the connection was established, loss follows the supervision rule */
	}
	if (central_seen) {
		return miss_count >= FOLLOW_ESTABLISH_GIVE_UP_MISS;
	}
	return miss_count >= FOLLOW_NEVER_HEARD_MISS;
}

/* ---- Channel-map policy for an encrypted link ----
 * After encryption LL_CHANNEL_MAP_IND is invisible. The old behaviour switched to guarding the
 * unmapped channel as soon as encryption started (a packet is only received there if the unmapped
 * channel happens to be in the peer's map): a phone often enables only 10 channels to coexist with
 * WiFi, so the hit rate immediately drops to 10/37 ~= 27%, and the LL_ENC_RSP (SKDs/IVs) that
 * follows LL_ENC_REQ is likely lost, so the session key cannot be computed (2026-10-11 live capture
 * 2222.pcapng: across five connections the events caught before encryption contained remapped
 * channels, but after encryption everything landed only on unmapped channels).
 *
 * New behaviour: keep trusting the known map after encryption; only start probing after missing up
 * to FOLLOW_MAP_PROBE_AFTER_MISS (the widened relock window has already tried a few rounds with no
 * catch), alternating every two events between "old map / unmapped channels" (in pairs, offset from
 * relock's per-event 1M/2M alternation so each map is tried with both PHYs). If a packet is received
 * on an unmapped channel during probing and that channel is not in the old map = the peer changed
 * its map -> switch to unmapped-guarding mode (map_trusted=false, wait for the host's key hint to
 * correct it); a packet received per the old map = the map is unchanged, clear miss and leave probing.
 */
#define FOLLOW_MAP_PROBE_AFTER_MISS 6u

/** Should this event probe on unmapped channels (rather than the known map)? */
static inline bool follow_policy_map_probe_unmapped(bool encrypted, bool map_trusted,
						    uint16_t miss_count)
{
	if (!encrypted || !map_trusted || miss_count < FOLLOW_MAP_PROBE_AFTER_MISS) {
		return false;
	}
	return (((miss_count - FOLLOW_MAP_PROBE_AFTER_MISS) >> 1) & 1u) != 0u;
}

/** After a packet is received during probing: does it prove the peer changed its map? */
static inline bool follow_policy_map_changed(bool probed_unmapped, bool unmapped_in_old_map)
{
	return probed_unmapped && !unmapped_in_old_map;
}

#endif /* FOLLOW_POLICY_H_ */
