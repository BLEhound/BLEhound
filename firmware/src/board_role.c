/*
 * Board role identification implementation — see board_role.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "board_role.h"
#include "radio_hal.h"   /* BLE_CHANNEL_ADV_37/38/39 */

LOG_MODULE_REGISTER(board_role, LOG_LEVEL_INF);

/* Power-up settling: after the internal pull-up (~13k) is enabled the pin's parasitic capacitance needs µs to charge to a high level; reading right after configuring may
 * misread a floating pin as 0, and a fly-wire strap may bounce. So wait a short moment first, then require two consecutive identical reads before trusting the value. */
#define STRAP_SETTLE_US   100
#define STRAP_READ_RETRY  5

/* The two straps are provided by DT aliases tri-strap0 / tri-strap1 (see the board overlay).
 * The DK has no such alias group — check with DT_NODE_EXISTS and take the fallback path if absent. */
#define STRAP0_NODE DT_ALIAS(tri_strap0)
#define STRAP1_NODE DT_ALIAS(tri_strap1)

#if DT_NODE_EXISTS(STRAP0_NODE) && DT_NODE_EXISTS(STRAP1_NODE)
#define TRI_STRAPS_PRESENT 1
static const struct gpio_dt_spec strap0 = GPIO_DT_SPEC_GET(STRAP0_NODE, gpios);
static const struct gpio_dt_spec strap1 = GPIO_DT_SPEC_GET(STRAP1_NODE, gpios);
#else
#define TRI_STRAPS_PRESENT 0
#endif

#if TRI_STRAPS_PRESENT
/* strap encoding (S1,S0) → role. ch uses the advertising-channel index. */
static struct board_role role_from_code(uint8_t code)
{
	switch (code) {
	case 0x0:  /* 00 = node A */
		return (struct board_role){ .board_id = 0, .guard_channel = BLE_CHANNEL_ADV_37 };
	case 0x2:  /* 10 = node B */
		return (struct board_role){ .board_id = 1, .guard_channel = BLE_CHANNEL_ADV_38 };
	case 0x1:  /* 01 = node C */
		return (struct board_role){ .board_id = 2, .guard_channel = BLE_CHANNEL_ADV_39 };
	default:   /* 11 reserved */
		LOG_WRN("strap combination 0b%u%u reserved, falling back to {id=0, ch=37}",
			(code >> 1) & 1, code & 1);
		return (struct board_role){ .board_id = 0, .guard_channel = BLE_CHANNEL_ADV_37 };
	}
}
#endif /* TRI_STRAPS_PRESENT */

struct board_role board_role_read(void)
{
#if TRI_STRAPS_PRESENT
	if (!gpio_is_ready_dt(&strap0) || !gpio_is_ready_dt(&strap1)) {
		LOG_WRN("strap GPIO not ready, falling back to {id=0, ch=37}");
		return (struct board_role){ .board_id = 0, .guard_channel = BLE_CHANNEL_ADV_37 };
	}

	/* Input + pull-up: a strap tied to GND reads 0, floating/pulled-up reads 1. Use ACTIVE_HIGH semantics
	 * so that gpio_pin_get_dt returns the level directly (high=1 low=0). */
	(void)gpio_pin_configure_dt(&strap0, GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_HIGH);
	(void)gpio_pin_configure_dt(&strap1, GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_HIGH);

	k_busy_wait(STRAP_SETTLE_US);
	int s0 = gpio_pin_get_dt(&strap0);
	int s1 = gpio_pin_get_dt(&strap1);

	for (int i = 0; i < STRAP_READ_RETRY; i++) {
		k_busy_wait(STRAP_SETTLE_US);
		const int r0 = gpio_pin_get_dt(&strap0);
		const int r1 = gpio_pin_get_dt(&strap1);

		if (r0 == s0 && r1 == s1) {
			break;   /* two consecutive reads agree, trust it */
		}
		s0 = r0;
		s1 = r1;
	}

	if (s0 < 0 || s1 < 0) {
		LOG_WRN("strap read failed (s0=%d s1=%d), falling back to {id=0, ch=37}", s0, s1);
		return (struct board_role){ .board_id = 0, .guard_channel = BLE_CHANNEL_ADV_37 };
	}

	const uint8_t code = (uint8_t)(((s1 & 1) << 1) | (s0 & 1));
	const struct board_role r = role_from_code(code);

	LOG_INF("role: board_id=%u guard_channel=%u (strap S1S0=%u%u)",
		r.board_id, r.guard_channel, (code >> 1) & 1, code & 1);
	return r;
#else
	/* No strap alias (e.g. single-board DK): run with single-board defaults — board_id 0, channel 37. */
	LOG_INF("no strap alias defined, using single-board default {id=0, ch=37}");
	return (struct board_role){ .board_id = 0, .guard_channel = BLE_CHANNEL_ADV_37 };
#endif
}
