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

#endif /* FOLLOW_POLICY_H_ */
