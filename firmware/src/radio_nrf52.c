/*
 * Radio HAL —— nRF52840 implementation
 *
 * Takes over RADIO / TIMER1 / PPI directly, bypassing the Zephyr BLE controller (which this project does not enable).
 *
 * Key design points:
 *   - Stop once per received packet (END_DISABLE), swap the buffer in the interrupt, then manually
 *     RXEN to restart.
 *     ⚠️ Do not use the END_START short for "continuous reception": on the nRF52 PACKETPTR is
 *     latched at TASKS_START, so the short would let the radio start writing the next packet into
 *     the same memory **before** the interrupt swaps the buffer, and the packet read out gets
 *     overwritten and corrupted (observed in practice as a ~45% CRC error rate and bit shifts in
 *     the AdvA of the same device). The cost is a blind spot of one ramp-up per packet (~40µs in
 *     fast mode), far shorter than T_IFS (150µs), so it does not affect back-to-back packets like
 *     SCAN_REQ/SCAN_RSP.
 *   - Double buffering: PACKETPTR is only switched after the radio stops, so the buffer the
 *     callback is processing is never touched by hardware.
 *   - µs timestamps: PPI wires the RADIO ADDRESS event straight to TIMER1's CAPTURE, so it is
 *     hardware-timestamped and unaffected by interrupt latency.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <nrfx.h>

#include "radio_hal.h"

/* RADIO interrupt priority. 0 is reserved for zero-latency IRQs in Zephyr, so use 1 (next highest) here. */
#define RADIO_IRQ_PRIORITY   1

/* PPI channel: RADIO EVENTS_ADDRESS → TIMER1 CAPTURE[1].
 * This project does not enable the BLE controller, so all PPI is idle; claiming one channel is fine. */
#define PPI_CH_TIMESTAMP     0
#define TIMER_CC_TIMESTAMP   1
/* TIMER1 CC[0] is dedicated to compare scheduling for connection following (see radio_sched_at) */
#define TIMER_CC_SCHED       0
#define TIMER_IRQ_PRIORITY   1

static radio_sched_cb_t sched_cb;

/* Packet layout in RAM: S0(1) + LENGTH(1) + payload. CRC is not written to RAM; it is read from the RXCRC register. */
#define RADIO_BUF_SIZE       RADIO_PDU_MAX_LEN

static uint8_t rx_buf[2][RADIO_BUF_SIZE] __aligned(4);
static uint8_t rx_idx;
static radio_packet_cb_t packet_cb;
static volatile uint8_t cur_channel;
/* current_phy = the currently configured PHY (used for the packet about to be received).
 * On an asymmetric link (e.g. C→P Coded, P→C 1M), the master and slave packets in one event have
 * different PHYs, alternated packet by packet via primary/alt: primary = master direction,
 * alt = slave direction. Equal means symmetric, no alternation. */
static volatile phy_t current_phy = PHY_1M;
static volatile phy_t primary_phy = PHY_1M;
static volatile phy_t alt_phy = PHY_1M;
static volatile uint8_t rx_pkt_idx;   /* packet index within the event: even=master, odd=slave */
static bool rx_running;

/*
 * BLE channel index → RADIO.FREQUENCY (MHz offset relative to 2400MHz).
 * Advertising channels 37/38/39 are 2402 / 2426 / 2480 MHz respectively, wedged in between the
 * data channels, so the mapping is not contiguous. See BT Core Spec Vol 6, Part B, §1.4.1.
 */
static uint8_t ble_channel_to_freq_offset(uint8_t ch)
{
	switch (ch) {
	case BLE_CHANNEL_ADV_37:
		return 2;    /* 2402 MHz */
	case BLE_CHANNEL_ADV_38:
		return 26;   /* 2426 MHz */
	case BLE_CHANNEL_ADV_39:
		return 80;   /* 2480 MHz */
	default:
		break;
	}

	/* Data channels 0..10 → 2404..2424; 11..36 → 2428..2478 */
	return (ch <= 10) ? (4 + 2 * ch) : (6 + 2 * ch);
}

static void radio_apply_phy(phy_t phy);

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (NRF_RADIO->EVENTS_END == 0) {
		return;
	}

	NRF_RADIO->EVENTS_END = 0;
	/* Read it back once: the Cortex-M4 write buffer delays the event clear from taking effect; without the read we would re-enter the interrupt */
	(void)NRF_RADIO->EVENTS_END;

	uint8_t *buf = rx_buf[rx_idx];

	/* At this point the radio has been stopped by the END_DISABLE short, so the metadata is stable */
	const bool crc_ok = (NRF_RADIO->CRCSTATUS == RADIO_CRCSTATUS_CRCSTATUS_CRCOk);
	/* RSSISAMPLE is a positive magnitude; the actual dBm is its negation */
	const int8_t rssi_dbm = -(int8_t)(NRF_RADIO->RSSISAMPLE & 0x7F);
	const uint32_t ts_us = NRF_TIMER1->CC[TIMER_CC_TIMESTAMP];
	/* CRC does not go into RAM and can only be read from the register —— the host needs it to assemble the complete BLE link-layer frame */
	const uint32_t rx_crc = NRF_RADIO->RXCRC & 0x00FFFFFFUL;
	/* This packet was received with the current PHY; note it down first (we may switch to another PHY below to receive the next packet) */
	const phy_t pkt_phy = current_phy;

	/* Asymmetric link: after receiving this packet, switch PHY for the next. Within an event, 0/2/4…
	 * are the master (primary), 1/3… are the slave (alt). The radio is stopped by END_DISABLE at
	 * this point, so MODE can be changed safely; the slave does not transmit until T_IFS (150µs)
	 * later, enough to complete the PHY switch + RXEN ramp (~40µs). On a symmetric link alt==primary,
	 * so no switch is triggered. */
	rx_pkt_idx++;
	if (alt_phy != primary_phy) {
		radio_apply_phy((rx_pkt_idx & 1) ? alt_phy : primary_phy);
	}

	/* Switch the landing spot to the other buffer first, then restart reception —— reversing the order would overwrite the buffer currently being read */
	rx_idx ^= 1;
	NRF_RADIO->PACKETPTR = (uint32_t)rx_buf[rx_idx];

	if (rx_running) {
		NRF_RADIO->TASKS_RXEN = 1;
	}

	if (packet_cb == NULL) {
		return;
	}

	const struct radio_packet pkt = {
		.pdu = buf,
		/* buf[1] is the LENGTH field (payload length); the total PDU length adds the two header bytes */
		.pdu_len = RADIO_PDU_HEADER_LEN + buf[1],
		.crc = rx_crc,
		.rssi_dbm = rssi_dbm,
		.channel = cur_channel,
		.phy = pkt_phy,
		.crc_ok = crc_ok,
		.timestamp_us = ts_us,
	};

	packet_cb(&pkt);
}

/* Free-running 1MHz counter, used to µs-timestamp received packets and as the scheduling time base for connection following */
static void timestamp_timer_init(void)
{
	NRF_TIMER1->TASKS_STOP = 1;
	NRF_TIMER1->MODE = TIMER_MODE_MODE_Timer;
	NRF_TIMER1->BITMODE = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
	NRF_TIMER1->PRESCALER = 4;   /* 16MHz / 2^4 = 1MHz, 1 tick = 1µs */
	NRF_TIMER1->TASKS_CLEAR = 1;
	NRF_TIMER1->TASKS_START = 1;
}

/* TIMER1 interrupt: only handles the CC[0] compare (scheduling). CC[1] is a PPI hardware capture and
 * produces no interrupt. Both entry paths dispatch directly: (1) the CC[0] compare actually fires;
 * (2) radio_sched_at finds the scheduled time has already passed and pends this interrupt manually.
 * So we must not return early just because EVENTS_COMPARE==0. */
static void timer1_isr(const void *arg)
{
	ARG_UNUSED(arg);

	NRF_TIMER1->EVENTS_COMPARE[TIMER_CC_SCHED] = 0;
	(void)NRF_TIMER1->EVENTS_COMPARE[TIMER_CC_SCHED];
	NRF_TIMER1->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk;

	radio_sched_cb_t cb = sched_cb;

	sched_cb = NULL;
	if (cb != NULL) {
		cb();
	}
}

uint32_t radio_now_us(void)
{
	NRF_TIMER1->TASKS_CAPTURE[3] = 1;   /* borrow CC[3] to read the current value, without clashing with scheduling/timestamp */
	return NRF_TIMER1->CC[3];
}

void radio_sched_at(uint32_t at_us, radio_sched_cb_t cb)
{
	sched_cb = cb;

	/* Ordering point: clear the event first, then write CC. Done the other way, a compare that fires
	 * exactly between the two writes would be cleared by the later step, and this interrupt would be lost. */
	NRF_TIMER1->EVENTS_COMPARE[TIMER_CC_SCHED] = 0;
	(void)NRF_TIMER1->EVENTS_COMPARE[TIMER_CC_SCHED];
	NRF_TIMER1->CC[TIMER_CC_SCHED] = at_us;
	NRF_TIMER1->INTENSET = TIMER_INTENSET_COMPARE0_Msk;

	/* Fallback: if the target time has already passed when CC is written, this compare would not fire
	 * until the 32-bit counter wraps all the way around (~71 minutes at 1MHz) —— the scheduling chain
	 * breaks on the spot and the whole sniffer stops receiving. Re-check after writing: if it has
	 * indeed passed and the event has not latched, pend the interrupt manually to dispatch once. */
	if ((int32_t)(at_us - radio_now_us()) <= 0 &&
	    NRF_TIMER1->EVENTS_COMPARE[TIMER_CC_SCHED] == 0) {
		NVIC_SetPendingIRQ(TIMER1_IRQn);
	}
}

void radio_sched_cancel(void)
{
	NRF_TIMER1->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk;
	NRF_TIMER1->EVENTS_COMPARE[TIMER_CC_SCHED] = 0;
	sched_cb = NULL;
	/* Clearing the peripheral event does not retract the one already pending in the NVIC —— without
	 * clearing it, a callback rescheduled right after cancellation would be dispatched once early by this leftover interrupt. */
	NVIC_ClearPendingIRQ(TIMER1_IRQn);
}

static void timestamp_ppi_init(void)
{
	NRF_PPI->CH[PPI_CH_TIMESTAMP].EEP = (uint32_t)&NRF_RADIO->EVENTS_ADDRESS;
	NRF_PPI->CH[PPI_CH_TIMESTAMP].TEP =
		(uint32_t)&NRF_TIMER1->TASKS_CAPTURE[TIMER_CC_TIMESTAMP];
	NRF_PPI->CHENSET = (1UL << PPI_CH_TIMESTAMP);
}

/* PHY-independent part of PCNF0: S0=1-byte LL header, LENGTH=8 bit, no S1 */
#define PCNF0_COMMON \
	((1UL << RADIO_PCNF0_S0LEN_Pos) | \
	 (8UL << RADIO_PCNF0_LFLEN_Pos) | \
	 (0UL << RADIO_PCNF0_S1LEN_Pos))

/*
 * Set MODE and PCNF0 (preamble length / coding indicator) per PHY.
 *   1M   : 1-byte preamble, MODE=Ble_1Mbit
 *   2M   : 2-byte preamble, MODE=Ble_2Mbit (only used after a connection PHY update)
 *   Coded: long preamble + CI(2 bit) + TERM(3 bit), MODE=Ble_LR125Kbit
 *          —— on reception this MODE auto-decodes S=8 or S=2 from the CI in the packet, one setting covering both.
 */
static void radio_apply_phy(phy_t phy)
{
	switch (phy) {
	case PHY_2M:
		NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_2Mbit << RADIO_MODE_MODE_Pos;
		NRF_RADIO->PCNF0 = PCNF0_COMMON |
			((uint32_t)RADIO_PCNF0_PLEN_16bit << RADIO_PCNF0_PLEN_Pos);
		break;
	case PHY_CODED_S8:
	case PHY_CODED_S2:
		NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_LR125Kbit << RADIO_MODE_MODE_Pos;
		NRF_RADIO->PCNF0 = PCNF0_COMMON |
			((uint32_t)RADIO_PCNF0_PLEN_LongRange << RADIO_PCNF0_PLEN_Pos) |
			(2UL << RADIO_PCNF0_CILEN_Pos) |
			(3UL << RADIO_PCNF0_TERMLEN_Pos);
		break;
	case PHY_1M:
	default:
		NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_1Mbit << RADIO_MODE_MODE_Pos;
		NRF_RADIO->PCNF0 = PCNF0_COMMON |
			((uint32_t)RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);
		break;
	}
	current_phy = phy;
}

static void radio_configure(void)
{
	radio_apply_phy(PHY_1M);

	/* BALEN=3 → 3-byte BASE + 1-byte PREFIX = 4-byte access address */
	NRF_RADIO->PCNF1 =
		/* MAXLEN limits the payload length (an 8-bit field), not counting the header/length two bytes */
		((uint32_t)RADIO_PAYLOAD_MAX_LEN << RADIO_PCNF1_MAXLEN_Pos) |
		(0UL << RADIO_PCNF1_STATLEN_Pos) |
		(3UL << RADIO_PCNF1_BALEN_Pos) |
		((uint32_t)RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
		((uint32_t)RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

	/* BLE CRC: 24 bit, polynomial x^24+x^10+x^9+x^6+x^4+x^3+x+1, excluding the access address */
	NRF_RADIO->CRCCNF =
		((uint32_t)RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
		((uint32_t)RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
	NRF_RADIO->CRCPOLY = 0x0000065BUL;

	NRF_RADIO->RXADDRESSES = 1UL;   /* only enable logical address 0 */

	NRF_RADIO->MODECNF0 =
		((uint32_t)RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
		((uint32_t)RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

	NRF_RADIO->INTENSET = RADIO_INTENSET_END_Msk;
}

/* The radio must run on the external 32MHz crystal; the internal RC is not accurate enough */
static int hfclk_start(void)
{
	static struct onoff_client cli;
	struct onoff_manager *mgr;
	int err, res;

	mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (mgr == NULL) {
		return -ENODEV;
	}

	sys_notify_init_spinwait(&cli.notify);

	err = onoff_request(mgr, &cli);
	if (err < 0) {
		return err;
	}

	while (sys_notify_fetch_result(&cli.notify, &res) == -EAGAIN) {
		k_yield();
	}

	return res;
}

int radio_init(radio_packet_cb_t cb)
{
	int err;

	if (cb == NULL) {
		return -EINVAL;
	}

	err = hfclk_start();
	if (err < 0) {
		return err;
	}

	packet_cb = cb;

	timestamp_timer_init();
	radio_configure();
	timestamp_ppi_init();

	IRQ_CONNECT(RADIO_IRQn, RADIO_IRQ_PRIORITY, radio_isr, NULL, 0);
	irq_enable(RADIO_IRQn);

	IRQ_CONNECT(TIMER1_IRQn, TIMER_IRQ_PRIORITY, timer1_isr, NULL, 0);
	irq_enable(TIMER1_IRQn);

	return 0;
}

static bool valid_phy(phy_t phy)
{
	return phy == PHY_1M || phy == PHY_2M ||
	       phy == PHY_CODED_S8 || phy == PHY_CODED_S2;
}

int radio_set_phy(phy_t phy)
{
	if (!valid_phy(phy)) {
		return -EINVAL;
	}
	primary_phy = phy;
	alt_phy = phy;   /* symmetric by default; for asymmetric, call radio_set_alt_phy after this */
	radio_apply_phy(phy);
	return 0;
}

int radio_set_alt_phy(phy_t phy)
{
	if (!valid_phy(phy)) {
		return -EINVAL;
	}
	alt_phy = phy;   /* slave-direction PHY; if it differs from primary, alternate packet by packet within the event */
	return 0;
}

phy_t radio_get_phy(void)
{
	return current_phy;
}

int radio_set_channel(uint8_t ble_channel)
{
	if (ble_channel > BLE_CHANNEL_MAX) {
		return -EINVAL;
	}

	NRF_RADIO->FREQUENCY = ble_channel_to_freq_offset(ble_channel);
	/* The whitening seed is just the channel index; register bit6 is hardwired to 1, no need to set it ourselves */
	NRF_RADIO->DATAWHITEIV = ble_channel;
	cur_channel = ble_channel;

	return 0;
}

static uint32_t current_aa;

void radio_set_access_addr(uint32_t aa)
{
	current_aa = aa;
	NRF_RADIO->BASE0 = (aa << 8) & 0xFFFFFF00UL;
	NRF_RADIO->PREFIX0 = (NRF_RADIO->PREFIX0 & ~RADIO_PREFIX0_AP0_Msk) |
			     ((aa >> 24) & RADIO_PREFIX0_AP0_Msk);
}

uint32_t radio_get_access_addr(void)
{
	return current_aa;
}

void radio_set_crcinit(uint32_t crc_init)
{
	NRF_RADIO->CRCINIT = crc_init & 0x00FFFFFFUL;
}

int radio_rx_start(void)
{
	if (rx_running) {
		return 0;
	}

	rx_idx = 0;
	NRF_RADIO->PACKETPTR = (uint32_t)rx_buf[0];

	/* Each start of reception begins on the master-direction PHY (packet 0 within the event = master) */
	rx_pkt_idx = 0;
	if (current_phy != primary_phy) {
		radio_apply_phy(primary_phy);
	}

	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;

	/* READY_START: auto-start reception once ramp-up completes; END_DISABLE: auto ramp-down after
	 * reception so the interrupt can swap the buffer safely (see the file header); ADDRESS_RSSISTART: sample RSSI as soon as the address matches. */
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_END_DISABLE_Msk |
			    RADIO_SHORTS_ADDRESS_RSSISTART_Msk;

	rx_running = true;
	NRF_RADIO->TASKS_RXEN = 1;

	return 0;
}

void radio_rx_stop(void)
{
	if (!rx_running) {
		return;
	}

	/* Clear the flag before stopping: otherwise an interrupt in flight would pull reception back up */
	rx_running = false;
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_DISABLE = 1;

	while (NRF_RADIO->EVENTS_DISABLED == 0) {
		/* ramp-down takes only a few tens of µs, busy-wait is fine */
	}

	NRF_RADIO->EVENTS_DISABLED = 0;
}
