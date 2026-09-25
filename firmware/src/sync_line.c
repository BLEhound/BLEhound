/*
 * SYNC line implementation (portable, Zephyr GPIO) — see sync_line.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <hal/nrf_gpio.h>   /* diagnostics: read the IN/PIN_CNF registers back directly */
#include <hal/nrf_gpiote.h>  /* diagnostics: read the GPIOTE channel config / event flags directly */

#include "sync_line.h"
#include "radio_hal.h"   /* radio_now_us(): same time base as the receive timestamps */

LOG_MODULE_REGISTER(sync_line, LOG_LEVEL_INF);

/* Open-drain pulse width: wide enough for the peer to sample the edge reliably, yet not holding the bus too long. */
#define SYNC_PULSE_US 3

#define SYNC_IN_NODE  DT_ALIAS(tri_sync_in)
#define SYNC_OUT_NODE DT_ALIAS(tri_sync_out)

#if DT_NODE_EXISTS(SYNC_IN_NODE) && DT_NODE_EXISTS(SYNC_OUT_NODE)
#define TRI_SYNC_PRESENT 1
static const struct gpio_dt_spec sync_in = GPIO_DT_SPEC_GET(SYNC_IN_NODE, gpios);
static const struct gpio_dt_spec sync_out = GPIO_DT_SPEC_GET(SYNC_OUT_NODE, gpios);
static struct gpio_callback sync_cb_data;
static sync_capture_cb_t user_cb;
static atomic_t last_tick;   /* most recently captured tick */
static bool single_pin;      /* full board: in/out share one pin (software open-drain); DK: two pins */
/* single-pin mode: the GPIO port base address and bit mask used for diagnostic read-back, computed from DT at init */
#define SYNC_OUT_PORT_NUM DT_PROP(DT_GPIO_CTLR(SYNC_OUT_NODE, gpios), port)
static NRF_GPIO_Type *sync_reg;
static uint32_t sync_mask;
static uint32_t sync_pin_idx;
static atomic_t capture_count;   /* number of edges captured by the ISR (for diagnostics) */
static uint32_t dbg_low_seen;    /* last pulse: the level read from the input buffer while driven low */
static uint32_t dbg_high_seen;   /* last pulse: the level read after release */
/* GPIOTE instance register base for the GPIO port this pin belongs to (gpio0→gpiote30); read channel config directly during diagnostics */
#define SYNC_GPIOTE_REG \
	((NRF_GPIOTE_Type *)DT_REG_ADDR(DT_PHANDLE(DT_GPIO_CTLR(SYNC_OUT_NODE, gpios), gpiote_instance)))
#else
#define TRI_SYNC_PRESENT 0
#endif

#if TRI_SYNC_PRESENT
static void sync_edge_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Edge arrived: immediately stamp a tick in this board's time base. Interrupt latency introduces µs-level jitter (see the sync_line.h notes). */
	const uint32_t tick = radio_now_us();

	atomic_set(&last_tick, (atomic_val_t)tick);
	atomic_inc(&capture_count);

	if (user_cb != NULL) {
		user_cb(tick);
	}
}
#endif

int sync_line_init(sync_capture_cb_t on_capture)
{
#if TRI_SYNC_PRESENT
	if (!gpio_is_ready_dt(&sync_in) || !gpio_is_ready_dt(&sync_out)) {
		LOG_WRN("SYNC GPIO not ready, disabling the sync line");
		return -ENODEV;
	}

	user_cb = on_capture;

	int err;

	single_pin = (sync_in.port == sync_out.port && sync_in.pin == sync_out.pin);

	if (single_pin) {
		/* Full board: in/out share one pin (P0.04/TD3, fly-wire interconnected). Configure it as **pure input + pull-up** (no always-on open-drain output).
		 * Reason: P0.04 is bound to gpiote30, which does not support port-event (SENSE) interrupts; the edge interrupt takes the IN-event channel path
		 * (which gpiote30 supports) only when the pin has DIR=INPUT. If configured as open-drain (DIR=OUTPUT),
		 * it is forced onto the SENSE path → gpio_pin_interrupt_configure returns -ENOTSUP (-134).
		 * So use software open-drain instead: normally input (high-Z + pull-up = released), and at emit() temporarily switch to output to drive low, then switch back. */
		err = gpio_pin_configure_dt(&sync_out, GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW);
		if (err != 0) {
			LOG_ERR("failed to configure SYNC single pin (pure input + pull-up): %d", err);
			return err;
		}
		/* Record the port register base / bit mask so diagnostics can read IN/PIN_CNF back directly (the pulse itself goes through the gpio API, see emit). */
		uint32_t abs_pin = NRF_GPIO_PIN_MAP(SYNC_OUT_PORT_NUM, sync_out.pin);

		sync_reg = nrf_gpio_pin_port_decode(&abs_pin);   /* side effect: abs_pin becomes the in-port bit number */
		sync_pin_idx = abs_pin;
		sync_mask = BIT(abs_pin);
	} else {
		/* Two pins (the finalized full-board scheme; the DK regression also jumpers two pins together): the output pin is open-drain, released high (wired-AND relies on the input pin's
		 * internal pull-up or an external pull-up); the input pin carries the edge interrupt. The output pin **must not** have any GPIOTE channel, otherwise on
		 * nRF54L GPIO writes get ignored by GPIOTE (see the SYNC notes in boards/common/blehound_nrf54lm20a_cpuapp_common.dtsi). */
		err = gpio_pin_configure_dt(&sync_out, GPIO_OUTPUT_INACTIVE | GPIO_OPEN_DRAIN |
							   GPIO_ACTIVE_LOW);
		if (err != 0) {
			LOG_ERR("failed to configure SYNC output: %d", err);
			return err;
		}

		/* Input: triggered on the falling edge (the instant the line is pulled low = the sync moment). Under ACTIVE_LOW semantics this is "becoming active". */
		err = gpio_pin_configure_dt(&sync_in, GPIO_INPUT | GPIO_ACTIVE_LOW);
		if (err != 0) {
			LOG_ERR("failed to configure SYNC input: %d", err);
			return err;
		}
	}
	err = gpio_pin_interrupt_configure_dt(&sync_in, GPIO_INT_EDGE_TO_ACTIVE);
	if (err != 0) {
		LOG_ERR("failed to configure SYNC interrupt: %d", err);
		return err;
	}

	gpio_init_callback(&sync_cb_data, sync_edge_isr, BIT(sync_in.pin));
	gpio_add_callback(sync_in.port, &sync_cb_data);

	LOG_INF("SYNC line ready (falling-edge capture of the common time base)");

	if (!single_pin) {
		/* One-time boot loopback self-test: drive the output pin low, the input pin should read 0 — verifies the output↔input pin jumper is in place (real-board header pin8↔pin10),
		 * and that the input pin can capture (this board's own edge also enters the ISR, so capture_count becoming 1 is normal). */
		gpio_pin_set_dt(&sync_out, 1);
		k_busy_wait(200);
		const int in_low = gpio_pin_get_dt(&sync_in);   /* ACTIVE_LOW: line low = 1 */

		gpio_pin_set_dt(&sync_out, 0);
		if (in_low == 1) {
			LOG_INF("SYNC loopback self-test passed: output pin driven low, input pin reads low (jumper in place)");
		} else {
			LOG_ERR("SYNC loopback self-test failed: output pin driven low but input pin reads %d — check the output↔input pin jumper (real-board header pin8↔pin10)",
				in_low);
		}
	}

	if (single_pin) {
		/* One-time boot hardware self-test: push-pull drive low + input buffer connected, read back after the input synchronizes. On nRF54L, once this pin
		 * has carried a GPIOTE IN event, GPIO writes get ignored (read 1), so the single-pin scheme is unusable on nRF54L; it works on nRF52. Done only once at boot: it pulls the shared line low for ~200µs, so other boards may capture a spurious edge,
		 * which is harmless during boot; it must not be repeated at runtime. When done, switch back to input and re-arm the interrupt (a driver reconfigure tears down the trigger). */
		(void)gpio_pin_configure_dt(&sync_out, GPIO_OUTPUT_LOW | GPIO_INPUT);
		k_busy_wait(200);
		const uint32_t in_low = nrf_gpio_port_in_read(sync_reg) & sync_mask;

		(void)gpio_pin_configure_dt(&sync_out, GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW);
		err = gpio_pin_interrupt_configure_dt(&sync_in, GPIO_INT_EDGE_TO_ACTIVE);
		if (in_low || err != 0) {
			LOG_ERR("SYNC self-test failed: pin reads %u when push-pull driven low (should be 0), re-arm interrupt=%d — "
				"on nRF54L a pin that has carried GPIOTE has ineffective GPIO writes, the single-pin scheme is unusable, use two pins instead",
				in_low ? 1U : 0U, err);
		} else {
			LOG_INF("SYNC self-test passed: this board can drive the pin low");
		}
	}

#ifdef SYNC_HOLD_LOW_TEST
	if (!single_pin) {
		gpio_pin_set_dt(&sync_out, 1);
		LOG_WRN("[test] SYNC output pin held low, please measure the header voltage");
	}
	/* Hardware-debug only (-DEXTRA_CFLAGS=-DSYNC_HOLD_LOW_TEST): hold the SYNC pin push-pull low permanently
	 * so a multimeter can measure the header voltage. No pulses are sent in this mode. The SYNC pin has already carried a GPIOTE interrupt by now (control group:
	 * the never-armed P0.04/P1.07 in log_debug below), and read back at several delays to see whether the drive-low is merely slow. */
	if (single_pin) {
		const int e = gpio_pin_configure_dt(&sync_out, GPIO_OUTPUT_LOW | GPIO_INPUT);
		static const uint32_t delays_us[] = {3, 200, 2000, 50000, 500000};
		uint32_t seen[ARRAY_SIZE(delays_us)];
		uint32_t elapsed = 0;

		for (size_t i = 0; i < ARRAY_SIZE(delays_us); i++) {
			k_busy_wait(delays_us[i] - elapsed);
			elapsed = delays_us[i];
			seen[i] = (nrf_gpio_port_in_read(sync_reg) & sync_mask) ? 1U : 0U;
		}
		LOG_WRN("[test] SYNC pin P%u.%02u (interrupt-armed) held push-pull low (cfg=%d), read-back @3us=%u @200us=%u @2ms=%u @50ms=%u @500ms=%u  PIN_CNF=0x%08x",
			SYNC_OUT_PORT_NUM, sync_out.pin, e, seen[0], seen[1], seen[2], seen[3], seen[4],
			sync_reg->PIN_CNF[sync_pin_idx]);
	}
#endif
	return 0;
#else
	ARG_UNUSED(on_capture);
	LOG_INF("tri-sync alias undefined, SYNC line disabled (single-board / degraded to coarse ordering by arrival)");
	return 0;
#endif
}

void sync_line_emit(void)
{
#if TRI_SYNC_PRESENT
#ifdef SYNC_HOLD_LOW_TEST
	if (single_pin) {
		return;   /* test mode: pin held low, no pulses sent */
	}
#endif
	if (single_pin) {
		/* Single-pin software open-drain: temporarily configure as "open-drain output low + input buffer connected" to drive low → hold → switch back to pure input + pull-up to release.
		 * Open-drain only pulls low and never drives high, so the three boards' shared line is wired-AND with no conflict.
		 *
		 * Two pitfalls (measured on the real board 2026-09-19):
		 *  1) Zephyr gpio_nrfx tears down the pin's edge trigger and releases the GPIOTE channel on every pin reconfigure
		 *     ("Remove previously configured trigger when pin is reconfigured"), so at the end of the pulse
		 *     gpio_pin_interrupt_configure_dt() must be called again to re-arm it, otherwise this board never receives SYNC again.
		 *  2) nRF54L GPIO has a RETAIN latch: writing OUTCLR/DIRSET directly, bypassing the driver, is blocked by retention
		 *     (OUT still reads back 1, the pin is driven high instead of low). Go through gpio_pin_configure_dt() so the driver handles it with the
		 *     retain_clear → write → retain_set sequence; measured, OUT then clears to 0 correctly.
		 *
		 * The trigger is torn down during the pulse, so this board cannot capture its own edge; the emitting board therefore stamps its own tick at the same instant it drives low
		 * (consistent with the design: the emitting board also stamps a tick in its own time base); other boards capture it in their own ISRs, with error = ISR latency (µs-level). */
		const uint32_t tick = radio_now_us();

		atomic_set(&last_tick, (atomic_val_t)tick);
		(void)gpio_pin_configure_dt(&sync_out, GPIO_OUTPUT_LOW | GPIO_OPEN_DRAIN | GPIO_INPUT);
		k_busy_wait(SYNC_PULSE_US);
		const uint32_t low_seen = nrf_gpio_port_in_read(sync_reg) & sync_mask;

		(void)gpio_pin_configure_dt(&sync_out, GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW);
		(void)gpio_pin_interrupt_configure_dt(&sync_in, GPIO_INT_EDGE_TO_ACTIVE);
		k_busy_wait(SYNC_PULSE_US);
		dbg_low_seen = low_seen ? 1U : 0U;
		dbg_high_seen = (nrf_gpio_port_in_read(sync_reg) & sync_mask) ? 1U : 0U;
	} else {
		/* DK two pins: always-on open-drain, assert (drive low) → hold → release (back high). Under ACTIVE_LOW, set(1) = pull low. */
		gpio_pin_set_dt(&sync_out, 1);
		k_busy_wait(SYNC_PULSE_US);
		gpio_pin_set_dt(&sync_out, 0);
	}
#endif
}

void sync_line_log_debug(void)
{
#if TRI_SYNC_PRESENT
	if (!single_pin) {
		LOG_INF("SYNC diag: input pin currently reads %d (ACTIVE_LOW, 0=line high) captures %ld",
			gpio_pin_get_dt(&sync_in), (long)atomic_get(&capture_count));
		return;
	}
	LOG_INF("SYNC diag: PIN_CNF=0x%08x current IN=%u last pulse drive-low read %u/release read %u captures %ld",
		sync_reg->PIN_CNF[sync_pin_idx],
		(nrf_gpio_port_in_read(sync_reg) & sync_mask) ? 1U : 0U,
		dbg_low_seen, dbg_high_seen, (long)atomic_get(&capture_count));

#ifdef SYNC_HOLD_LOW_TEST
	LOG_WRN("[test] holding low: PIN_CNF=0x%08x OUT=%u DIR=%u IN=%u (expected 0)",
		sync_reg->PIN_CNF[sync_pin_idx],
		(nrf_gpio_port_out_read(sync_reg) & sync_mask) ? 1U : 0U,
		(nrf_gpio_port_dir_read(sync_reg) & sync_mask) ? 1U : 0U,
		(nrf_gpio_port_in_read(sync_reg) & sync_mask) ? 1U : 0U);

	/* Control group: never-armed (no GPIOTE interrupt) P0.04 (TD3), P1.07 (TCLK) held low and read back */
	{
		static bool cand_init;
		const struct device *g0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
		const struct device *g1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

		if (!cand_init) {
			cand_init = true;
			(void)gpio_pin_configure(g0, 4, GPIO_OUTPUT_LOW | GPIO_INPUT);
			(void)gpio_pin_configure(g1, 7, GPIO_OUTPUT_LOW | GPIO_INPUT);
		}
		LOG_WRN("[test] control (never interrupt-armed) held low: P0.04 (TD3, pin10) IN=%u  P1.07 (TCLK, pin2) IN=%u",
			nrf_gpio_pin_read(NRF_GPIO_PIN_MAP(0, 4)), nrf_gpio_pin_read(NRF_GPIO_PIN_MAP(1, 7)));
	}

	return;
#endif
	NRF_GPIOTE_Type *te = SYNC_GPIOTE_REG;

	for (uint32_t i = 0; i < ARRAY_SIZE(te->CONFIG); i++) {
		if (!nrf_gpiote_te_is_enabled(te, i)) {
			continue;
		}
		LOG_INF("  gpiote ch%u: CONFIG=0x%08x pin=%u pol=%u EVENTS_IN=%u INTEN=%u",
			i, te->CONFIG[i], nrf_gpiote_event_pin_get(te, i),
			nrf_gpiote_event_polarity_get(te, i),
			nrf_gpiote_event_check(te, nrf_gpiote_in_event_get(i)) ? 1U : 0U,
			nrf_gpiote_int_enable_check(te, BIT(i)) ? 1U : 0U);
	}
#endif
}

uint32_t sync_line_capture_count(void)
{
#if TRI_SYNC_PRESENT
	return (uint32_t)atomic_get(&capture_count);
#else
	return 0;
#endif
}

uint32_t sync_line_last_capture(void)
{
#if TRI_SYNC_PRESENT
	return (uint32_t)atomic_get(&last_tick);
#else
	return 0;
#endif
}
