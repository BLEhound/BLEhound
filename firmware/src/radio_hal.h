/*
 * Radio HAL -- the radio abstraction layer used for sniffing (the porting contract)
 *
 * This layer is the only place in the entire sniffer that touches the chip's radio registers.
 * Switching chips (nRF52840 → nRF54L15/L20) only requires writing another implementation; the upper
 * layers sniffer_core / conn_follower stay unchanged.
 *
 * Design constraints:
 *   - Zephyr's BLE controller is not enabled; RADIO / TIMER / PPI are owned exclusively by this layer;
 *   - the packet-RX callback runs in **interrupt context**, so the implementation only copies and
 *     enqueues -- no blocking or logging allowed.
 */

#ifndef RADIO_HAL_H_
#define RADIO_HAL_H_

#include <stdbool.h>
#include <stdint.h>

/** BLE PHY. P0 implements only 1M; the rest are left for P2. */
typedef enum {
	PHY_1M = 0,
	PHY_2M,
	PHY_CODED_S8,
	PHY_CODED_S2,
} phy_t;

/** Fixed parameters of the advertising channels (BT Core Spec Vol 6, Part B) */
#define BLE_ADV_ACCESS_ADDR   0x8E89BED6U
#define BLE_ADV_CRC_INIT      0x555555U

/** BLE channel index range: 0..36 are data channels, 37/38/39 are advertising channels */
#define BLE_CHANNEL_MAX       39
#define BLE_CHANNEL_ADV_37    37
#define BLE_CHANNEL_ADV_38    38
#define BLE_CHANNEL_ADV_39    39

/** Maximum BLE link-layer payload length */
#define RADIO_PAYLOAD_MAX_LEN 255

/** Complete LL PDU = header(1) + length(1) + payload */
#define RADIO_PDU_HEADER_LEN  2
#define RADIO_PDU_MAX_LEN     (RADIO_PDU_HEADER_LEN + RADIO_PAYLOAD_MAX_LEN)

/**
 * A received packet. The buffer pointed to by pdu is valid only during the callback; the implementer must copy it.
 *
 * pdu is the **complete LL PDU** (including the header and length bytes) -- the host concatenates the
 * access address + this PDU + crc to form the BLE link-layer frame that Wireshark wants, so no
 * splitting is done here.
 */
struct radio_packet {
	const uint8_t *pdu;      /**< LL PDU: header(1) + length(1) + payload */
	uint16_t pdu_len;        /**< total PDU length = 2 + payload length */
	uint32_t crc;            /**< the raw received 24-bit CRC value (for the host to rebuild the complete frame) */
	int8_t rssi_dbm;         /**< received signal strength, negative value */
	uint8_t channel;         /**< BLE channel index 0..39 */
	uint8_t phy;             /**< the PHY on which this packet was received (phy_t): 0=1M 1=2M 2/3=Coded */
	bool crc_ok;             /**< whether the CRC check passed (failed packets are also reported, for diagnostics) */
	uint32_t timestamp_us;   /**< the moment the access address was received, µs, counted from radio_init */
};

/**
 * Packet-RX callback. **Called in interrupt context**, must be extremely short: only copy the data and enqueue.
 */
typedef void (*radio_packet_cb_t)(const struct radio_packet *pkt);

/**
 * Initialize the radio: request the HFXO, configure RADIO/TIMER/PPI, register the interrupt.
 *
 * @return 0 on success, a negative errno on failure
 */
int radio_init(radio_packet_cb_t cb);

/**
 * Set the PHY. Supports 1M / 2M / Coded (on receive, the single Coded setting auto-decodes S8/S2 by the CI).
 */
int radio_set_phy(phy_t phy);

/** Read back the current PHY -- the host uses it to annotate which PHY a packet was received on when framing */
phy_t radio_get_phy(void);

/**
 * Set the "peripheral-direction" PHY (for asymmetric links). When it differs from the central PHY
 * set by radio_set_phy, packets alternate within a connection event: central packets (0/2/4…) use
 * the PHY from radio_set_phy, peripheral packets (1/3…) use the PHY set here. If they are equal there
 * is no alternation (symmetric). radio_set_phy resets it to be the same as the central PHY, so this
 * must be called **after** radio_set_phy.
 */
int radio_set_alt_phy(phy_t phy);

/**
 * Set the BLE channel index (0..39).
 *
 * Note: this function sets **both** the RF frequency and the whitening seed -- both are uniquely
 * determined by the channel index, and splitting them into two interfaces would only create the
 * mismatch of "set the frequency but forgot the whitening". radio_set_whitening() in design document
 * §5.1 is therefore merged into this function.
 *
 * @return 0 on success, -EINVAL if the channel number is out of range
 */
int radio_set_channel(uint8_t ble_channel);

/** Set the access address (use BLE_ADV_ACCESS_ADDR for advertising, the value from CONNECT_IND for a connection) */
void radio_set_access_addr(uint32_t aa);

/** Read back the current access address -- the host uses it to annotate which link a packet belongs to when framing */
uint32_t radio_get_access_addr(void);

/** Set the CRC init value (fixed BLE_ADV_CRC_INIT for advertising, the CRCInit from CONNECT_IND for a connection) */
void radio_set_crcinit(uint32_t crc_init);

/** Start receiving. Received packets keep invoking the callback until radio_rx_stop(). */
int radio_rx_start(void);

/** Stop receiving. */
void radio_rx_stop(void);

/* ---- Timed scheduling (for connection following) --------------------------
 *
 * Connection following needs to switch channels and open the receive window at precise moments.
 * This reuses the same TIMER1 used for timestamping (free-running at 1MHz) for one-shot compare
 * scheduling -- triggered by a hardware interrupt, the jitter is only on the order of interrupt
 * latency (a few µs), far better than a Zephyr software timer (which can be hundreds of µs).
 */

/** The callback fired when the scheduled time is reached; runs in interrupt context, must be extremely short. */
typedef void (*radio_sched_cb_t)(void);

/** Read the current TIMER1 count (µs), on the same time base as the packet-RX timestamps. */
uint32_t radio_now_us(void);

/**
 * One-shot scheduling: when the TIMER1 count reaches at_us, call cb in the interrupt.
 * Calling again overrides the previous one (a single timing line). at_us uses the absolute time base and may wrap around.
 */
void radio_sched_at(uint32_t at_us, radio_sched_cb_t cb);

/** Cancel a scheduled callback that has not yet fired. */
void radio_sched_cancel(void);

#endif /* RADIO_HAL_H_ */
