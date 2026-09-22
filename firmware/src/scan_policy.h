/*
 * Scan channel policy -- pure function, no hardware dependency, called from conn_follower and
 * unit-tested on the host.
 *
 * In three-unit mode (design §5.1) each unit sticks to the advertising channel specified by its strap
 * and does not hop (scan_hopping=false). Single-board mode is allowed to hop among 37/38/39. Every
 * "switch advertising channel" decision goes through here, to keep some code path from forgetting to
 * check scan_hopping and quietly hopping the guarded channel away.
 */

#ifndef SCAN_POLICY_H_
#define SCAN_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#include "radio_hal.h"   /* BLE_CHANNEL_ADV_37/38/39 */

/** Rotate 37→38→39→37; a non-advertising channel falls back to 37. */
static inline uint8_t scan_next_adv_channel(uint8_t ch)
{
	switch (ch) {
	case BLE_CHANNEL_ADV_37:
		return BLE_CHANNEL_ADV_38;
	case BLE_CHANNEL_ADV_38:
		return BLE_CHANNEL_ADV_39;
	case BLE_CHANNEL_ADV_39:
		return BLE_CHANNEL_ADV_37;
	default:
		return BLE_CHANNEL_ADV_37;
	}
}

/**
 * Which advertising channel to use when returning to scanning during a gap in connection following:
 * advance one step if hopping is allowed, keep it unchanged in stick mode.
 */
static inline uint8_t scan_channel_after_gap(uint8_t ch, bool hopping)
{
	return hopping ? scan_next_adv_channel(ch) : ch;
}

#endif /* SCAN_POLICY_H_ */
