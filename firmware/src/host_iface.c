/*
 * Host interface implementation —— see host_iface.h for the frame format.
 *
 * Transmission uses a "ring buffer + TX interrupt" scheme rather than blocking writes: when the
 * host has not read in time, a blocking write would stall the calling thread and let the capture
 * queue back up. The sniffer's trade-off is clear —— **better to drop packets than to slow down
 * the radio path** —— so when the buffer is full it drops the frame and reports the count.
 *
 * The ring buffer is single-producer (sniffer thread), single-consumer (CDC interrupt); Zephyr's
 * ring_buf needs no extra locking in this usage.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

/* The USB stack splits into two variants by chip: nRF52 uses the legacy usb_device_stack, while
 * the nRF54L DesignWare usbhs is only supported by the new UDC/usbd stack. CDC ACM appears as a
 * UART device in both stacks, so the RX/TX/DTR code below is common; only the USB-enable path is
 * split by stack with #if. */
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
#include <zephyr/usb/usbd.h>
#include <sample_usbd.h>
#else
#include <zephyr/usb/usb_device.h>
#endif

#include "host_iface.h"
#include "cobs.h"

LOG_MODULE_REGISTER(host_iface, LOG_LEVEL_INF);

/* TX ring buffer. 8KB buffers roughly 150 full-length advertising frames, enough to absorb jitter on the USB side. */
#define TX_RINGBUF_SIZE       8192

/* Maximum length of one frame before encoding (including the possible 5-byte tri-device extension) */
#define FRAME_RAW_MAX         (HOST_FRAME_HEADER_LEN + HOST_TRI_EXT_LEN + RADIO_PDU_MAX_LEN)
/* After COBS encoding + 1 delimiter byte */
#define FRAME_ENCODED_MAX     (COBS_ENCODED_MAX(FRAME_RAW_MAX) + 1)

/* Buffer for receiving commands: commands are all short, so 64 bytes is more than enough */
#define CMD_BUF_SIZE          64

RING_BUF_DECLARE(tx_ringbuf, TX_RINGBUF_SIZE);

static const struct device *const cdc_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);
static host_cmd_cb_t command_cb;
static uint32_t dropped_frames;

/* Command receive state: accumulate until the 0x00 delimiter, then decode the whole frame */
static uint8_t cmd_buf[CMD_BUF_SIZE];
static size_t cmd_len;
static bool cmd_overflow;

/* ------------------------------------------------------------ CDC interrupt */

static void cdc_tx_pump(const struct device *dev)
{
	uint8_t *data;
	uint32_t claimed = ring_buf_get_claim(&tx_ringbuf, &data, TX_RINGBUF_SIZE);

	if (claimed == 0) {
		/* No more data to send; disable the TX interrupt and re-enable it on the next send */
		uart_irq_tx_disable(dev);
		ring_buf_get_finish(&tx_ringbuf, 0);
		return;
	}

	const int sent = uart_fifo_fill(dev, data, claimed);

	ring_buf_get_finish(&tx_ringbuf, (sent > 0) ? sent : 0);
}

static void cmd_frame_complete(void)
{
	uint8_t decoded[CMD_BUF_SIZE];

	/* Drop overflowed frames outright: commands are all short, so overlength means the stream is already garbled */
	if (!cmd_overflow && cmd_len > 0) {
		const size_t n = cobs_decode(cmd_buf, cmd_len, decoded, sizeof(decoded));

		if (n >= 1 && command_cb != NULL) {
			command_cb(decoded[0], &decoded[1], (uint8_t)(n - 1));
		}
	}

	cmd_len = 0;
	cmd_overflow = false;
}

static void cdc_rx_pump(const struct device *dev)
{
	uint8_t buf[64];
	const int n = uart_fifo_read(dev, buf, sizeof(buf));

	for (int i = 0; i < n; i++) {
		if (buf[i] == 0x00) {
			cmd_frame_complete();
			continue;
		}

		if (cmd_len < sizeof(cmd_buf)) {
			cmd_buf[cmd_len++] = buf[i];
		} else {
			cmd_overflow = true;
		}
	}
}

static void cdc_irq_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			cdc_rx_pump(dev);
		}

		if (uart_irq_tx_ready(dev)) {
			cdc_tx_pump(dev);
		}
	}
}

/* ---------------------------------------------------------------- USB enable */

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)

static struct usbd_context *usbd_ctx;

/* Device-event callback for the new stack. The nRF54L usbhs can detect VBUS —— only usbd_enable
 * once the device USB cable is plugged in (VBUS arrives), and disable when it is unplugged. This
 * way it does not block when unplugged and comes online automatically when plugged in. */
static void usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	if (!usbd_can_detect_vbus(ctx)) {
		return;
	}
	if (msg->type == USBD_MSG_VBUS_READY) {
		if (usbd_enable(ctx)) {
			LOG_ERR("usbd_enable failed");
		}
	} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
		(void)usbd_disable(ctx);
	}
}

static int usb_bringup(void)
{
	usbd_ctx = sample_usbd_init_device(usbd_msg_cb);
	if (usbd_ctx == NULL) {
		LOG_ERR("usbd init failed");
		return -ENODEV;
	}

	/* On platforms that support VBUS detection (nRF54L usbhs), enable later in the VBUS_READY
	 * callback; on those that do not, enable now. */
	if (!usbd_can_detect_vbus(usbd_ctx)) {
		const int err = usbd_enable(usbd_ctx);

		if (err != 0) {
			LOG_ERR("usbd_enable failed: %d", err);
			return err;
		}
	}
	return 0;
}

#else /* legacy usb_device_stack (nRF52) */

static int usb_bringup(void)
{
	const int err = usb_enable(NULL);

	if (err != 0) {
		LOG_ERR("usb_enable failed: %d", err);
		return err;
	}
	return 0;
}

#endif

/* ---------------------------------------------------------------- API */

int host_iface_init(host_cmd_cb_t cmd_cb)
{
	if (!device_is_ready(cdc_dev)) {
		LOG_ERR("CDC ACM device not ready");
		return -ENODEV;
	}

	const int err = usb_bringup();

	if (err != 0) {
		return err;
	}

	command_cb = cmd_cb;

	uart_irq_callback_set(cdc_dev, cdc_irq_handler);
	uart_irq_rx_enable(cdc_dev);

	LOG_INF("USB CDC ACM ready (capture data channel)");

	return 0;
}

bool host_iface_connected(void)
{
	uint32_t dtr = 0;

	(void)uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr);

	return dtr != 0;
}

/*
 * Shared implementation for framing + COBS + enqueueing into the ring buffer. When tri=false this
 * is the legacy board frame; when tri=true it inserts 5 bytes (board_id + sync_epoch) after the
 * fixed header, sets HOST_FLAG_TRI, and shifts pdu back by 5 bytes accordingly. Both paths share
 * the same framing logic so the pdu offset / COBS / drop policy are not written twice and drift apart.
 */
static int send_frame(const struct radio_packet *pkt, uint32_t access_addr,
		      uint8_t phy, bool tri, uint8_t board_id, uint32_t sync_epoch)
{
	uint8_t raw[FRAME_RAW_MAX];
	uint8_t encoded[FRAME_ENCODED_MAX];

	if (!host_iface_connected()) {
		return -ENOTCONN;
	}

	const uint8_t pdu_len = (uint8_t)MIN(pkt->pdu_len, RADIO_PDU_MAX_LEN);

	raw[0] = HOST_FRAME_PACKET;
	raw[1] = (pkt->crc_ok ? HOST_FLAG_CRC_OK : 0) | (tri ? HOST_FLAG_TRI : 0);
	sys_put_le32(pkt->timestamp_us, &raw[2]);
	raw[6] = pkt->channel;
	raw[7] = (uint8_t)pkt->rssi_dbm;
	raw[8] = phy;
	sys_put_le32(access_addr, &raw[9]);
	raw[13] = (uint8_t)(pkt->crc & 0xFF);
	raw[14] = (uint8_t)((pkt->crc >> 8) & 0xFF);
	raw[15] = (uint8_t)((pkt->crc >> 16) & 0xFF);
	raw[16] = pdu_len;

	size_t off = HOST_FRAME_HEADER_LEN;

	if (tri) {
		raw[off] = board_id;
		sys_put_le32(sync_epoch, &raw[off + 1]);
		off += HOST_TRI_EXT_LEN;
	}

	memcpy(&raw[off], pkt->pdu, pdu_len);

	const size_t raw_len = off + pdu_len;
	size_t enc_len = cobs_encode(raw, raw_len, encoded);

	encoded[enc_len++] = 0x00;   /* frame delimiter */

	/* If it does not fit, drop the whole frame —— never write only half, that would corrupt the frame boundary for every frame after it */
	if (ring_buf_space_get(&tx_ringbuf) < enc_len) {
		dropped_frames++;
		return -ENOMEM;
	}

	ring_buf_put(&tx_ringbuf, encoded, enc_len);
	uart_irq_tx_enable(cdc_dev);

	return 0;
}

int host_iface_send_packet(const struct radio_packet *pkt, uint32_t access_addr,
			   uint8_t phy)
{
	return send_frame(pkt, access_addr, phy, false, 0, 0);
}

int host_iface_send_packet_tri(const struct radio_packet *pkt, uint32_t access_addr,
			       uint8_t phy, uint8_t board_id, uint32_t sync_epoch)
{
	return send_frame(pkt, access_addr, phy, true, board_id, sync_epoch);
}

uint32_t host_iface_dropped(void)
{
	return dropped_frames;
}
