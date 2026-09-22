/*
 * LED indication policy -- pure function, no hardware dependency, called from main and unit-tested
 * on the host.
 *
 * A single LED reflects only **this chip's own** RF following state, unrelated to SYNC or to the
 * other boards (when three units follow together, each shows its own):
 *
 *   Scanning (not following a connection): normally off, on for LED_IDLE_BLINK_MS every
 *                       LED_IDLE_PERIOD_MS -- alive and guarding the advertising channel.
 *   Following, packets on air:  toggle the level once per tick as long as the ACL slot received a
 *                       packet -- the blink rhythm is the connection's rhythm (a connection with an
 *                       interval ≥ 2 ticks shows a per-event toggle; a 7.5ms fast connection becomes a 25Hz flicker).
 *   Following, no packets on air:  after LED_RF_SILENT_MS with no packets, stay on -- the slot is
 *                       still locked but receiving nothing (re-locking / peer silent).
 *   Lost:               when the ACL slot is released due to supervision timeout, blink fast
 *                       LED_LOST_BLINKS times, on/off LED_LOST_HALF_MS each, then return to whatever
 *                       state applies at that moment. A normal disconnect via the peer's
 *                       LL_TERMINATE_IND gives no indication.
 *
 * Time is uniformly in milliseconds (k_uptime_get_32), and all subtractions are compared with unsigned wraparound.
 */

#ifndef LED_POLICY_H_
#define LED_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#define LED_IDLE_PERIOD_MS   1000u   /* scanning heartbeat period */
#define LED_IDLE_BLINK_MS    50u     /* scanning heartbeat on-duration */
#define LED_RF_SILENT_MS     500u    /* how long with no packets while following before deciding "air silent" → stay on */
#define LED_LOST_BLINKS      3u      /* number of blinks for the lost indication */
#define LED_LOST_HALF_MS     100u    /* each half-period of the lost indication (on 100 / off 100 = 5Hz) */

enum led_state {
	LED_STATE_IDLE = 0,       /* scanning advertising, not following */
	LED_STATE_FOLLOW_RF,      /* following, packets on air (toggle per packet) */
	LED_STATE_FOLLOW_SILENT,  /* following, air silent (stay on) */
	LED_STATE_LOST,           /* lost indication (fast blink) */
};

struct led_policy {
	uint32_t packets_seen;     /* ACL received-packet count already consumed */
	uint32_t lost_seen;        /* lost count already consumed */
	bool was_following;        /* whether following on the previous tick (to detect idle→follow transition) */
	uint32_t last_rf_ms;       /* tick timestamp of the most recent received packet */
	bool level;                /* toggle level in the following state */
	uint32_t idle_blink_ms;    /* start of the current scanning heartbeat */
	uint32_t idle_next_ms;     /* start of the next scanning heartbeat */
	uint32_t lost_start_ms;    /* start of the lost indication */
	bool lost_active;          /* lost indication in progress */
};

/** Initialize. Pass the current values of the two counts: events before the policy is enabled do not count. */
static inline void led_policy_init(struct led_policy *p, uint32_t now_ms,
				   uint32_t initial_packets, uint32_t initial_lost)
{
	*p = (struct led_policy){
		.packets_seen = initial_packets,
		.lost_seen = initial_lost,
		.last_rf_ms = now_ms,
		.idle_blink_ms = now_ms - LED_IDLE_BLINK_MS,   /* has not blinked yet */
		.idle_next_ms = now_ms,                        /* fire the first heartbeat immediately: visible at power-on */
	};
}

static inline const char *led_state_name(enum led_state st)
{
	switch (st) {
	case LED_STATE_FOLLOW_RF:
		return "follow";
	case LED_STATE_FOLLOW_SILENT:
		return "follow-silent";
	case LED_STATE_LOST:
		return "lost";
	case LED_STATE_IDLE:
	default:
		return "scan";
	}
}

/**
 * Call once per tick.
 * @param active_acl  the number of currently active ACL connection slots (conn_follower_active_connections)
 * @param packets     cumulative CRC-OK packets received on ACL slots (conn_follower_acl_packets)
 * @param lost        cumulative ACL slot releases due to supervision timeout (conn_follower_acl_lost_timeouts)
 * @param state       outputs the current state (for logs/stats)
 * @return the LED level it should have (true = on)
 */
static inline bool led_policy_step(struct led_policy *p, uint32_t now_ms,
				   uint8_t active_acl, uint32_t packets, uint32_t lost,
				   enum led_state *state)
{
	const bool following = active_acl > 0;

	/* Lost indication: whenever the count changes, start a burst of fast blinking that overrides other displays */
	if (lost != p->lost_seen) {
		p->lost_seen = lost;
		p->lost_active = true;
		p->lost_start_ms = now_ms;
	}
	if (p->lost_active) {
		const uint32_t elapsed = now_ms - p->lost_start_ms;

		if (elapsed < LED_LOST_BLINKS * 2u * LED_LOST_HALF_MS) {
			*state = LED_STATE_LOST;
			return ((elapsed / LED_LOST_HALF_MS) & 1u) == 0u;   /* on-off-on-off-on-off */
		}
		p->lost_active = false;
		p->idle_next_ms = now_ms;   /* if returning to scanning after the indication ends, the heartbeat starts now */
		p->last_rf_ms = now_ms;     /* if still following, do not decide silent right away */
	}

	if (!following) {
		p->was_following = false;
		*state = LED_STATE_IDLE;
		if ((int32_t)(now_ms - p->idle_next_ms) >= 0) {
			p->idle_blink_ms = now_ms;
			p->idle_next_ms = now_ms + LED_IDLE_PERIOD_MS;
		}
		return (uint32_t)(now_ms - p->idle_blink_ms) < LED_IDLE_BLINK_MS;
	}

	if (!p->was_following) {
		/* Just started following: toggle from off, and start the silence timer from now */
		p->was_following = true;
		p->last_rf_ms = now_ms;
		p->level = false;
		p->packets_seen = packets;   /* there can be no ACL packets before the slot is set up; align just in case */
	}

	if (packets != p->packets_seen) {
		p->packets_seen = packets;
		p->last_rf_ms = now_ms;
		p->level = !p->level;
		*state = LED_STATE_FOLLOW_RF;
		return p->level;
	}
	if ((uint32_t)(now_ms - p->last_rf_ms) >= LED_RF_SILENT_MS) {
		p->level = true;   /* stay on; once packets resume, toggle starting from on */
		*state = LED_STATE_FOLLOW_SILENT;
		return true;
	}
	*state = LED_STATE_FOLLOW_RF;
	return p->level;
}

#endif /* LED_POLICY_H_ */
