/*
 * nRF21540 FEM pure-GPIO control implementation -- see fem_ctrl.h (includes the P0.09 rules, must read before changing).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "fem_ctrl.h"

LOG_MODULE_REGISTER(fem_ctrl, LOG_LEVEL_INF);

/* nRF21540 PS v1.0 Table 6 settling times (µs), matching the defaults of the zephyr nordic,nrf21540-fem binding.
 * TX is set to 26 = 11 (PG→TX) + margin for the SoC RF power ramp-up. ⚠️ On-board verification: may be tightened per measurement. */
#define FEM_PDN_SETTLE_US 18 /* PD → PG */
#define FEM_RX_SETTLE_US  11 /* PG → RX */
#define FEM_TX_SETTLE_US  26 /* PG → TX */
#define FEM_TRX_HOLD_US    3 /* RX/TX → PG */

#define FEM_PDN_NODE    DT_ALIAS(fem_pdn)
#define FEM_RXEN_NODE   DT_ALIAS(fem_rxen)
#define FEM_TXEN_NODE   DT_ALIAS(fem_txen)
#define FEM_ANTSEL_NODE DT_ALIAS(fem_antsel)
#define FEM_MODE_NODE   DT_ALIAS(fem_mode)
#define FEM_CSN_NODE    DT_ALIAS(fem_csn)
#define FEM_SCK_NODE    DT_ALIAS(fem_sck)
#define FEM_MOSI_NODE   DT_ALIAS(fem_mosi)

#if DT_NODE_EXISTS(FEM_PDN_NODE) && DT_NODE_EXISTS(FEM_RXEN_NODE) && \
	DT_NODE_EXISTS(FEM_TXEN_NODE) && DT_NODE_EXISTS(FEM_ANTSEL_NODE) && \
	DT_NODE_EXISTS(FEM_MODE_NODE) && DT_NODE_EXISTS(FEM_CSN_NODE) && \
	DT_NODE_EXISTS(FEM_SCK_NODE) && DT_NODE_EXISTS(FEM_MOSI_NODE)
#define FEM_PRESENT 1

static const struct gpio_dt_spec fem_pdn = GPIO_DT_SPEC_GET(FEM_PDN_NODE, gpios);
static const struct gpio_dt_spec fem_rxen = GPIO_DT_SPEC_GET(FEM_RXEN_NODE, gpios);
static const struct gpio_dt_spec fem_txen = GPIO_DT_SPEC_GET(FEM_TXEN_NODE, gpios);
static const struct gpio_dt_spec fem_antsel = GPIO_DT_SPEC_GET(FEM_ANTSEL_NODE, gpios);
static const struct gpio_dt_spec fem_mode = GPIO_DT_SPEC_GET(FEM_MODE_NODE, gpios);
static const struct gpio_dt_spec fem_csn = GPIO_DT_SPEC_GET(FEM_CSN_NODE, gpios);
static const struct gpio_dt_spec fem_sck = GPIO_DT_SPEC_GET(FEM_SCK_NODE, gpios);
static const struct gpio_dt_spec fem_mosi = GPIO_DT_SPEC_GET(FEM_MOSI_NODE, gpios);

/* Before power-up, configure each control pin to a defined state. Order: first "park" the SPI port, then RX/TX/antenna/gain, and PDN last. */
struct fem_pin_setup {
	const struct gpio_dt_spec *spec;
	gpio_flags_t flags;
	const char *what;
};

static const struct fem_pin_setup fem_pins[] = {
	/* CSN is ACTIVE_LOW in DT, INACTIVE = physical high = never selected → MISO (P0.09) stays high-Z forever. */
	{ &fem_csn, GPIO_OUTPUT_INACTIVE, "CSN high" },
	/* SCK/MOSI are not driven, only pulled down to a defined level: on the full board the FEM inputs
	 * do not float; on the DK, P0.07 is the uart30 RX direction (driven by the IMCU on the other side),
	 * and an input pull-down will not fight it. */
	{ &fem_sck, GPIO_INPUT | GPIO_PULL_DOWN, "SCK pull-down" },
	{ &fem_mosi, GPIO_INPUT | GPIO_PULL_DOWN, "MOSI pull-down" },
	/* Both RX and TX off first: the moment PDN goes high the FEM is in the PG state, then enable the LNA separately. */
	{ &fem_txen, GPIO_OUTPUT_INACTIVE, "TX_EN low" },
	{ &fem_rxen, GPIO_OUTPUT_INACTIVE, "RX_EN low" },
	/* ANT_SEL low = ANT1. The full-board ANT2 branch (C/L/MM8130) is DNP (not populated), so only ANT1 is usable. */
	{ &fem_antsel, GPIO_OUTPUT_INACTIVE, "ANT_SEL=ANT1" },
	/* MODE high = POUTB (+10 dBm factory preset), low = POUTA (+20 dBm). The firmware does not transmit,
	 * so take the conservative setting for this reserved path; change to GPIO_OUTPUT_INACTIVE for +20 dBm. */
	{ &fem_mode, GPIO_OUTPUT_ACTIVE, "MODE=POUTB(+10dBm)" },
	/* PDN low: first ensure the PD state, then drive it high at the end of init. */
	{ &fem_pdn, GPIO_OUTPUT_INACTIVE, "PDN low" },
};

static int fem_pins_configure(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(fem_pins); i++) {
		const struct fem_pin_setup *p = &fem_pins[i];

		if (!gpio_is_ready_dt(p->spec)) {
			LOG_ERR("FEM GPIO not ready: %s", p->what);
			return -ENODEV;
		}
		int err = gpio_pin_configure_dt(p->spec, p->flags);

		if (err != 0) {
			LOG_ERR("Failed to configure FEM %s: %d", p->what, err);
			return err;
		}
	}
	return 0;
}
#else
#define FEM_PRESENT 0
#endif

int fem_ctrl_init(void)
{
#if FEM_PRESENT
	int err = fem_pins_configure();

	if (err != 0) {
		return err;
	}

	/* PD → PG: power up, wait for internal stabilization. */
	gpio_pin_set_dt(&fem_pdn, 1);
	k_busy_wait(FEM_PDN_SETTLE_US);

	/* PG → RX: LNA always on, after which the radio can receive at any time. */
	gpio_pin_set_dt(&fem_rxen, 1);
	k_busy_wait(FEM_RX_SETTLE_US);

	LOG_INF("nRF21540 ready: pure GPIO, LNA always on, ANT1, CSN high (SPI disabled, P0.09 untouched)");
	return 0;
#else
	LOG_INF("no fem-* alias defined, no FEM (DK / single-board direct antenna)");
	return 0;
#endif
}

void fem_ctrl_set_tx(bool on)
{
#if FEM_PRESENT
	/* TX_EN/RX_EN must not be high at the same time: first fall back to PG (wait hold), then enter the target state (wait settle). */
	if (on) {
		gpio_pin_set_dt(&fem_rxen, 0);
		k_busy_wait(FEM_TRX_HOLD_US);
		gpio_pin_set_dt(&fem_txen, 1);
		k_busy_wait(FEM_TX_SETTLE_US);
	} else {
		gpio_pin_set_dt(&fem_txen, 0);
		k_busy_wait(FEM_TRX_HOLD_US);
		gpio_pin_set_dt(&fem_rxen, 1);
		k_busy_wait(FEM_RX_SETTLE_US);
	}
#else
	ARG_UNUSED(on);
#endif
}
