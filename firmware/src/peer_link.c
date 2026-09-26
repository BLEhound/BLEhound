/*
 * Inter-board link implementation -- see peer_link.h. Downstream master port: bit-banged SPI on GPIOs;
 * upstream slave port: nrfx_spis; REQ on GPIO.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>

#include "peer_link.h"
#include "peer_msg.h"

LOG_MODULE_REGISTER(peer_link, LOG_LEVEL_INF);

/* The master port needs: SCK/MOSI/MISO GPIOs (tri-spim-*) + CS (tri-peer-cs) + the two REQ lines. */
#if DT_NODE_EXISTS(DT_ALIAS(tri_spim_sck)) && DT_NODE_EXISTS(DT_ALIAS(tri_spim_mosi)) && \
	DT_NODE_EXISTS(DT_ALIAS(tri_spim_miso)) && DT_NODE_EXISTS(DT_ALIAS(tri_peer_cs)) && \
	DT_NODE_EXISTS(DT_ALIAS(tri_req_in)) && DT_NODE_EXISTS(DT_ALIAS(tri_req_out))
#define TRI_PEER_PRESENT 1
#else
#define TRI_PEER_PRESENT 0
#endif

/* The slave port needs: the four pin aliases + the spi21 node + the nrfx SPIS driver. */
#if TRI_PEER_PRESENT && defined(CONFIG_NRFX_SPIS) && DT_NODE_EXISTS(DT_NODELABEL(spi21)) && \
	DT_NODE_EXISTS(DT_ALIAS(tri_spis_sck)) && DT_NODE_EXISTS(DT_ALIAS(tri_spis_mosi)) && \
	DT_NODE_EXISTS(DT_ALIAS(tri_spis_miso)) && DT_NODE_EXISTS(DT_ALIAS(tri_spis_csn))
#define TRI_SPIS_PRESENT 1
#include <nrfx_spis.h>
#else
#define TRI_SPIS_PRESENT 0
#endif

/* DT gpio-keys entry -> nrfx pin number (port<<5 | pin) */
#define PIN_OF(alias) \
	NRF_GPIO_PIN_MAP(DT_PROP(DT_GPIO_CTLR(DT_ALIAS(alias), gpios), port), DT_GPIO_PIN(DT_ALIAS(alias), gpios))

static struct peer_link_stats g_stats;
static peer_recv_cb_t user_cb;

#if TRI_PEER_PRESENT

/* REQ pulse width: GPIOTE edge detection only needs a few clock cycles, 10us is plenty of margin. */
#define REQ_PULSE_US 10
/* Software SPI half period (us): ~400 kHz, one 33-byte message ~0.7 ms. SPIS does not care about clock jitter. */
#define SPIM_HALF_US 1

/* Master port pins. Why not SPIM00: on the nRF54LM20A SPIM00 only outputs SCK on the dedicated clock pin P2.01
 * (measured on real boards 2026-09-27: P2.02 never moved), and on the V1 board P2.01 is the old SYNC net shared by
 * all three chips -- so the port is bit-banged. Everything is configured/driven through raw nrfx GPIO (not the
 * Zephyr GPIO driver) so the nRF54L RETAIN latch cannot block the raw writes. */
static const uint32_t m_sck = PIN_OF(tri_spim_sck);
static const uint32_t m_mosi = PIN_OF(tri_spim_mosi);
static const uint32_t m_miso = PIN_OF(tri_spim_miso);
static const uint32_t m_cs = PIN_OF(tri_peer_cs);      /* active low */
static const struct gpio_dt_spec req_in = GPIO_DT_SPEC_GET(DT_ALIAS(tri_req_in), gpios);
static const struct gpio_dt_spec req_out = GPIO_DT_SPEC_GET(DT_ALIAS(tri_req_out), gpios);

static struct gpio_callback req_cb_data;
static struct k_work req_work;
static uint8_t spim_rx_buf[PEER_MSG_MAX];

static void spim_init(void)
{
	nrf_gpio_pin_clear(m_sck);
	nrf_gpio_cfg_output(m_sck);
	nrf_gpio_pin_clear(m_mosi);
	nrf_gpio_cfg_output(m_mosi);
	nrf_gpio_cfg_input(m_miso, NRF_GPIO_PIN_NOPULL);
	nrf_gpio_pin_set(m_cs);
	nrf_gpio_cfg_output(m_cs);
}

/* One transaction (SPI mode 0, MSB first): CS low -> per bit set MOSI, rising edge (peer samples, we read MISO on
 * the same edge), falling edge -> CS high. tx NULL sends zeros; rx NULL discards. Thread context (an interrupt in
 * the middle only stretches one bit, which SPIS does not mind). */
static void spim_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	nrf_gpio_pin_clear(m_cs);
	k_busy_wait(2);
	for (size_t i = 0; i < len; i++) {
		const uint8_t b = tx != NULL ? tx[i] : 0u;
		uint8_t r = 0;

		for (int bit = 7; bit >= 0; bit--) {
			if ((b >> bit) & 1u) {
				nrf_gpio_pin_set(m_mosi);
			} else {
				nrf_gpio_pin_clear(m_mosi);
			}
			k_busy_wait(SPIM_HALF_US);
			nrf_gpio_pin_set(m_sck);
			r = (uint8_t)((r << 1) | (nrf_gpio_pin_read(m_miso) ? 1u : 0u));
			k_busy_wait(SPIM_HALF_US);
			nrf_gpio_pin_clear(m_sck);
		}
		if (rx != NULL) {
			rx[i] = r;
		}
	}
	nrf_gpio_pin_clear(m_mosi);
	k_busy_wait(2);
	nrf_gpio_pin_set(m_cs);
}

/* The downstream slave pulled REQ_in: the interrupt only submits a work item; the thread then acts as master and
 * runs one fixed-length read to fetch the message the slave has queued. */
static void req_in_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_work_submit(&req_work);
}

static void req_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	spim_xfer(NULL, spim_rx_buf, sizeof(spim_rx_buf));
	g_stats.spim_rx++;
	if (user_cb != NULL) {
		user_cb(spim_rx_buf, sizeof(spim_rx_buf));
	}
}

#endif /* TRI_PEER_PRESENT */

#if TRI_SPIS_PRESENT

#define SPIS_NODE DT_NODELABEL(spi21)

static nrfx_spis_t spis = NRFX_SPIS_INSTANCE(DT_REG_ADDR(SPIS_NODE));
static uint8_t spis_rx_buf[PEER_MSG_MAX];
static uint8_t spis_tx_buf[PEER_MSG_MAX];   /* message waiting for the upstream master to read (send_upstream writes it) */

static void spis_arm(void)
{
	const int err = nrfx_spis_buffers_set(&spis, spis_tx_buf, sizeof(spis_tx_buf),
					      spis_rx_buf, sizeof(spis_rx_buf));

	if (err != 0) {
		g_stats.spis_arm_err++;
		g_stats.last_err = err;
	}
}

/* SPIS interrupt: after every transaction. A message pushed by the upstream master starts with its type byte; when
 * the master only reads us it pushes zeros, which is not a valid type and is ignored. Re-arm immediately so the
 * next transaction can be received at any time. */
static void spis_handler(nrfx_spis_event_t const *ev, void *ctx)
{
	ARG_UNUSED(ctx);

	if (ev->evt_type != NRFX_SPIS_XFER_DONE) {
		return;
	}
	g_stats.spis_xfer++;
	if (ev->rx_amount >= 3 && (spis_rx_buf[0] == PEER_MSG_HIT || spis_rx_buf[0] == PEER_MSG_HANDOFF) &&
	    user_cb != NULL) {
		g_stats.spis_rx++;
		user_cb(spis_rx_buf, ev->rx_amount);
	}
	memset(spis_rx_buf, 0, sizeof(spis_rx_buf));
	spis_arm();
}

static int spis_init(void)
{
	nrfx_spis_config_t cfg = NRFX_SPIS_DEFAULT_CONFIG(PIN_OF(tri_spis_sck), PIN_OF(tri_spis_mosi),
							  PIN_OF(tri_spis_miso), PIN_OF(tri_spis_csn));

	/* CS floats while the master resets: pull it up against false selects (the board has a 10k too, belt and
	 * braces); idle/over-read characters are 0, which the receiver treats as "no message". */
	cfg.csn_pullup = NRF_GPIO_PIN_PULLUP;
	cfg.def = 0;
	cfg.orc = 0;
	cfg.mode = NRF_SPIS_MODE_0;
	cfg.bit_order = NRF_SPIS_BIT_ORDER_MSB_FIRST;

	IRQ_CONNECT(DT_IRQN(SPIS_NODE), DT_IRQ(SPIS_NODE, priority), nrfx_spis_irq_handler, &spis, 0);

	const int err = nrfx_spis_init(&spis, &cfg, spis_handler, NULL);

	if (err != 0) {
		LOG_ERR("SPIS init failed: %d", err);
		g_stats.last_err = err;
		return err;
	}
	spis_arm();
	g_stats.spis_ready = g_stats.spis_arm_err == 0;
	return g_stats.spis_ready ? 0 : -EIO;
}

#endif /* TRI_SPIS_PRESENT */

int peer_link_init(peer_recv_cb_t on_recv)
{
	user_cb = on_recv;
#if TRI_PEER_PRESENT
	spim_init();
	k_work_init(&req_work, req_work_fn);

	/* REQ output: released. REQ input: edge interrupt triggers the master port read. */
	(void)gpio_pin_configure_dt(&req_out, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&req_in, GPIO_INPUT);
	(void)gpio_pin_interrupt_configure_dt(&req_in, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&req_cb_data, req_in_isr, BIT(req_in.pin));
	gpio_add_callback(req_in.port, &req_cb_data);

#if TRI_SPIS_PRESENT
	const int serr = spis_init();

	LOG_INF("inter-board link ready: software SPI master port (~%u kHz) + REQ, slave port SPIS %s",
		1000u / (2u * SPIM_HALF_US + 1u), serr == 0 ? "armed" : "FAILED");
#else
	LOG_INF("inter-board link ready: software SPI master port + REQ (no slave port on this board target)");
#endif
	return 0;
#else
	LOG_INF("tri-peer alias undefined, P3 inter-board coordination disabled (single-board or P1/P2 mode)");
	return 0;
#endif
}

int peer_link_send(const uint8_t *msg, size_t len)
{
#if TRI_PEER_PRESENT
	if (msg == NULL || len == 0U || len > PEER_MSG_MAX) {
		return -EINVAL;
	}
	spim_xfer(msg, NULL, len);
	g_stats.spim_tx++;
	return 0;
#else
	ARG_UNUSED(msg);
	ARG_UNUSED(len);
	return -ENOTSUP;
#endif
}

int peer_link_send_upstream(const uint8_t *msg, size_t len)
{
#if TRI_SPIS_PRESENT
	if (msg == NULL || len == 0U || len > PEER_MSG_MAX) {
		return -EINVAL;
	}
	if (!g_stats.spis_ready) {
		return -ENOTSUP;
	}

	/* The TX buffer pointer is already handed to EasyDMA and its contents are read only when the transaction
	 * happens; only the contents change here, not the armed state. */
	const unsigned int key = irq_lock();

	memset(spis_tx_buf, 0, sizeof(spis_tx_buf));
	memcpy(spis_tx_buf, msg, len);
	irq_unlock(key);

	/* Pull REQ_out (ACTIVE_LOW: set(1) = drive low) so the upstream master comes to read. */
	gpio_pin_set_dt(&req_out, 1);
	k_busy_wait(REQ_PULSE_US);
	gpio_pin_set_dt(&req_out, 0);
	g_stats.req_out++;
	return 0;
#else
	ARG_UNUSED(msg);
	ARG_UNUSED(len);
	return -ENOTSUP;
#endif
}

void peer_link_get_stats(struct peer_link_stats *out)
{
	if (out != NULL) {
		*out = g_stats;
	}
}
