/*
 * Inter-board link implementation — see peer_link.h. The outgoing-link master port uses the Zephyr SPI master API + GPIO REQ.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include "peer_link.h"
#include "peer_msg.h"

LOG_MODULE_REGISTER(peer_link, LOG_LEVEL_INF);

/* The inter-board link needs: one master-port SPI bus (tri-peer-spi) + CS (tri-peer-cs) + two REQ lines.
 * On the DK these are mapped by overlay onto real peripherals/GPIOs as placeholders for compile/link; repoint the aliases when the full board is fabricated. */
#define PEER_SPI_NODE DT_ALIAS(tri_peer_spi)

#if DT_NODE_EXISTS(PEER_SPI_NODE) && DT_NODE_HAS_STATUS(PEER_SPI_NODE, okay) && \
	DT_NODE_EXISTS(DT_ALIAS(tri_peer_cs)) && \
	DT_NODE_EXISTS(DT_ALIAS(tri_req_in)) && DT_NODE_EXISTS(DT_ALIAS(tri_req_out))
#define TRI_PEER_PRESENT 1
#else
#define TRI_PEER_PRESENT 0
#endif

#if TRI_PEER_PRESENT

/* REQ pulse width: wide enough for the peer to sample reliably. */
#define REQ_PULSE_US 3

static const struct device *const peer_bus = DEVICE_DT_GET(PEER_SPI_NODE);
static const struct gpio_dt_spec peer_cs = GPIO_DT_SPEC_GET(DT_ALIAS(tri_peer_cs), gpios);
static const struct gpio_dt_spec req_in = GPIO_DT_SPEC_GET(DT_ALIAS(tri_req_in), gpios);
static const struct gpio_dt_spec req_out = GPIO_DT_SPEC_GET(DT_ALIAS(tri_req_out), gpios);

static struct gpio_callback req_cb_data;
static peer_recv_cb_t user_cb;
static uint8_t rx_buf[PEER_MSG_MAX];

static struct spi_config peer_cfg;   /* cs filled in at runtime, see init */

/* The outgoing-link slave pulled REQ_in → this board acts as master and starts a read transaction to pull in the message the slave has queued.
 * ⚠️ Verify on hardware: the actual read length / whether a command byte must be written first depends on the slave (SPIS) convention; here we read a fixed length. */
static void req_in_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	const struct spi_buf rb = { .buf = rx_buf, .len = sizeof(rx_buf) };
	const struct spi_buf_set rxs = { .buffers = &rb, .count = 1 };

	if (spi_read(peer_bus, &peer_cfg, &rxs) == 0 && user_cb != NULL) {
		user_cb(rx_buf, sizeof(rx_buf));
	}
}

#endif /* TRI_PEER_PRESENT */

int peer_link_init(peer_recv_cb_t on_recv)
{
#if TRI_PEER_PRESENT
	if (!device_is_ready(peer_bus)) {
		LOG_WRN("inter-board SPI bus not ready, P3 coordination disabled");
		return -ENODEV;
	}

	user_cb = on_recv;

	peer_cfg.frequency = 1000000U;
	peer_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB;
	peer_cfg.cs.gpio = peer_cs;
	peer_cfg.cs.delay = 0U;

	/* REQ output: released state. REQ input: edge interrupt triggers the master-port read. */
	(void)gpio_pin_configure_dt(&req_out, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&req_in, GPIO_INPUT);
	(void)gpio_pin_interrupt_configure_dt(&req_in, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&req_cb_data, req_in_isr, BIT(req_in.pin));
	gpio_add_callback(req_in.port, &req_cb_data);

	LOG_INF("inter-board link ready (outgoing-link master port + REQ; incoming-link slave port SPIS pending on-board bring-up)");
	return 0;
#else
	ARG_UNUSED(on_recv);
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

	const struct spi_buf tb = { .buf = (void *)msg, .len = len };
	const struct spi_buf_set txs = { .buffers = &tb, .count = 1 };

	return spi_write(peer_bus, &peer_cfg, &txs);
#else
	ARG_UNUSED(msg);
	ARG_UNUSED(len);
	return -ENOTSUP;
#endif
}

void peer_link_request(void)
{
#if TRI_PEER_PRESENT
	/* As the slave: pull REQ_out to ask the incoming-link master to read. The SPIS must first be loaded with the outgoing message (on-board bring-up). */
	gpio_pin_set_dt(&req_out, 1);
	k_busy_wait(REQ_PULSE_US);
	gpio_pin_set_dt(&req_out, 0);
#endif
}
