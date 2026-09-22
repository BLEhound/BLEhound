/*
 * Radio HAL —— nRF54LM20A implementation (✅ compile + link verified; ⚠️ not verified on hardware)
 *
 * Verification status: the whole image (this file + all upper layers + USB) **compiles and links**
 *   for `nrf54lm20dk/nrf54lm20a/cpuapp`, producing zephyr.hex (FLASH 2.76% / RAM 6.93%). The upper
 *   layers (ble_csa / conn_follower / host_iface / main) compiled without a single line changed ——
 *   the direct payoff of abstracting the radio into a HAL.
 *
 * ⚠️ But "compiles" does not mean "works": no nRF54L hardware is on hand, so the **functional
 *   correctness** of the following points must be measured one by one on a board with an
 *   oscilloscope / logic analyzer / capture comparison (all marked "verify on hardware" in the code):
 *   1) **Interrupt lines**: INTENSET00 maps to RADIO_0_IRQn —— this pairing needs confirming;
 *   2) **DPPI topology**: RADIO and TIMER10 must sit on the same DPPIC domain; this file uses
 *      DPPIC10 by inference and must be checked against the datasheet's DPPI connection table;
 *   3) **TIMER base clock**: the divider assumes 16MHz for 1µs/tick; recompute if the base clock differs;
 *   4) **PLL ramp-up**: the blind-spot timing of restarting a PLL-type radio after reception needs measuring (does it drop back-to-back packets);
 *   5) **DATAWHITE.POLY**: confirm the reset polynomial is indeed the BLE whitening polynomial;
 *   6) **Clock**: confirm the HFXO request succeeds.
 *
 * Confirmed nRF54L vs nRF52 differences (nailed down via compiler/headers): END→PHYEND, single
 * INTENSET→multi-line INTENSET00, PPI→DPPI publish/subscribe, TIMER1→TIMER10/21, DATAWHITEIV→DATAWHITE
 * (adds a POLY field), MODECNF0 removed, RADIO_IRQn→RADIO_0/1_IRQn. See the P6 porting guide for details.
 *
 * CMakeLists selects this file based on CONFIG_SOC_SERIES_NRF54L.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <nrfx.h>

#include "radio_hal.h"

#define RADIO_IRQ_PRIORITY   1
#define TIMER_IRQ_PRIORITY   1

/* The DPPI publish/subscribe registers all use bit31 as the enable bit */
#define DPPI_PUBSUB_EN       (1UL << 31)

/*
 * ⚠️ Verify on hardware: the choice of TIMER instance and DPPIC instance.
 * The nRF54L has multiple TIMERs (TIMER00/10/20…) and multiple DPPICs (DPPIC00/10/20…),
 * and RADIO and the chosen TIMER must sit on **the same DPPIC domain** to be interconnected via DPPI.
 * The choice here (TIMER10 + DPPIC10) is inferred from a common topology and must be checked against
 * the "DPPI connections" table in the nRF54LM20A datasheet.
 */
#define SNIFF_TIMER          NRF_TIMER10
#define SNIFF_TIMER_IRQn     TIMER10_IRQn
#define SNIFF_DPPIC          NRF_DPPIC10
#define DPPI_CH_TIMESTAMP    0
#define TIMER_CC_TIMESTAMP   1
#define TIMER_CC_SCHED       0

/*
 * TIMER divider. Confirmed by measurement: the nRF54L TIMER base clock is **32MHz** (not the nRF52's 16MHz).
 * PRESCALER=4 (÷16) would give 2MHz, making timestamps/scheduling all run twice as fast —— that is exactly
 * how the µs-level anchor of connection following would drift off and not a single packet would be received.
 * PRESCALER=5 (÷32) gives 32MHz/32 = 1MHz = 1µs/tick.
 * (Debugging trail: a self-test 50ms measured ~103k ticks, should be 50k, ~2x too fast.)
 */
#define TIMER_PRESCALER      5

#define RADIO_BUF_SIZE       RADIO_PDU_MAX_LEN

static uint8_t rx_buf[2][RADIO_BUF_SIZE] __aligned(4);
static uint8_t rx_idx;
static radio_packet_cb_t packet_cb;
static volatile uint8_t cur_channel;
/* PHY state: on an asymmetric link, alternate packet by packet within the event (see the same logic in radio_nrf52.c) */
static volatile phy_t current_phy = PHY_1M;
static volatile phy_t primary_phy = PHY_1M;
static volatile phy_t alt_phy = PHY_1M;
static volatile uint8_t rx_pkt_idx;
static bool rx_running;
static uint32_t current_aa;

/* PHY-independent part of PCNF0: S0=1-byte LL header, LENGTH=8 bit, no S1 */
#define PCNF0_COMMON \
	((1UL << RADIO_PCNF0_S0LEN_Pos) | \
	 (8UL << RADIO_PCNF0_LFLEN_Pos) | \
	 (0UL << RADIO_PCNF0_S1LEN_Pos))

/* Set MODE and PCNF0 per PHY (nRF54L symbols; Coded uses LongRange+CI+TERM) */
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
static radio_sched_cb_t sched_cb;

/* BLE channel index → RADIO.FREQUENCY (offset relative to 2400MHz), same as nRF52 */
static uint8_t ble_channel_to_freq_offset(uint8_t ch)
{
	switch (ch) {
	case BLE_CHANNEL_ADV_37:
		return 2;
	case BLE_CHANNEL_ADV_38:
		return 26;
	case BLE_CHANNEL_ADV_39:
		return 80;
	default:
		break;
	}
	return (ch <= 10) ? (4 + 2 * ch) : (6 + 2 * ch);
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);

	/* nRF54L: the end of BLE reception is PHYEND, not the nRF52's END */
	if (NRF_RADIO->EVENTS_PHYEND == 0) {
		return;
	}
	NRF_RADIO->EVENTS_PHYEND = 0;
	(void)NRF_RADIO->EVENTS_PHYEND;

	uint8_t *buf = rx_buf[rx_idx];

	const bool crc_ok = (NRF_RADIO->CRCSTATUS == RADIO_CRCSTATUS_CRCSTATUS_CRCOk);
	const int8_t rssi_dbm = -(int8_t)(NRF_RADIO->RSSISAMPLE & 0x7F);
	const uint32_t ts_us = SNIFF_TIMER->CC[TIMER_CC_TIMESTAMP];
	const uint32_t rx_crc = NRF_RADIO->RXCRC & 0x00FFFFFFUL;
	const phy_t pkt_phy = current_phy;

	/* Asymmetric link: switch PHY for the next packet after receiving this one (0/2/4=master, 1/3=slave); alt==primary means no switch */
	rx_pkt_idx++;
	if (alt_phy != primary_phy) {
		radio_apply_phy((rx_pkt_idx & 1) ? alt_phy : primary_phy);
	}

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

static void timestamp_timer_init(void)
{
	SNIFF_TIMER->TASKS_STOP = 1;
	SNIFF_TIMER->MODE = TIMER_MODE_MODE_Timer;
	SNIFF_TIMER->BITMODE = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
	SNIFF_TIMER->PRESCALER = TIMER_PRESCALER;
	SNIFF_TIMER->TASKS_CLEAR = 1;
	SNIFF_TIMER->TASKS_START = 1;
}

/* Both entry paths dispatch directly: (1) the CC[0] compare actually fires; (2) radio_sched_at finds
 * the scheduled time has already passed and pends this interrupt manually. So we must not return early
 * just because EVENTS_COMPARE==0. */
static void timer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	SNIFF_TIMER->EVENTS_COMPARE[TIMER_CC_SCHED] = 0;
	(void)SNIFF_TIMER->EVENTS_COMPARE[TIMER_CC_SCHED];
	SNIFF_TIMER->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk;

	radio_sched_cb_t cb = sched_cb;

	sched_cb = NULL;
	if (cb != NULL) {
		cb();
	}
}

uint32_t radio_now_us(void)
{
	SNIFF_TIMER->TASKS_CAPTURE[3] = 1;
	return SNIFF_TIMER->CC[3];
}

void radio_sched_at(uint32_t at_us, radio_sched_cb_t cb)
{
	sched_cb = cb;

	/* Ordering point: clear the event first, then write CC. Done the other way, a compare that fires
	 * exactly between the two writes would be cleared by the later step, and this interrupt would be lost. */
	SNIFF_TIMER->EVENTS_COMPARE[TIMER_CC_SCHED] = 0;
	(void)SNIFF_TIMER->EVENTS_COMPARE[TIMER_CC_SCHED];
	SNIFF_TIMER->CC[TIMER_CC_SCHED] = at_us;
	SNIFF_TIMER->INTENSET = TIMER_INTENSET_COMPARE0_Msk;

	/* Fallback: if the target time has already passed when CC is written, this compare would not fire
	 * until the 32-bit counter wraps all the way around (~71 minutes at 1MHz) —— the scheduling chain
	 * breaks on the spot and the whole sniffer stops receiving. Re-check after writing: if it has
	 * indeed passed and the event has not latched, pend the interrupt manually to dispatch once. */
	if ((int32_t)(at_us - radio_now_us()) <= 0 &&
	    SNIFF_TIMER->EVENTS_COMPARE[TIMER_CC_SCHED] == 0) {
		NVIC_SetPendingIRQ(SNIFF_TIMER_IRQn);
	}
}

void radio_sched_cancel(void)
{
	SNIFF_TIMER->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk;
	SNIFF_TIMER->EVENTS_COMPARE[TIMER_CC_SCHED] = 0;
	sched_cb = NULL;
	/* Clearing the peripheral event does not retract the one already pending in the NVIC —— without
	 * clearing it, a callback rescheduled right after cancellation would be dispatched once early by this leftover interrupt. */
	NVIC_ClearPendingIRQ(SNIFF_TIMER_IRQn);
}

/* DPPI: the RADIO ADDRESS event is published to a channel, and the TIMER's CAPTURE task subscribes to it,
 * equivalent to the nRF52's "PPI: RADIO.EVENTS_ADDRESS → TIMER.TASKS_CAPTURE". */
static void timestamp_dppi_init(void)
{
	NRF_RADIO->PUBLISH_ADDRESS = DPPI_CH_TIMESTAMP | DPPI_PUBSUB_EN;
	SNIFF_TIMER->SUBSCRIBE_CAPTURE[TIMER_CC_TIMESTAMP] =
		DPPI_CH_TIMESTAMP | DPPI_PUBSUB_EN;
	SNIFF_DPPIC->CHENSET = (1UL << DPPI_CH_TIMESTAMP);
}

static void radio_configure(void)
{
	radio_apply_phy(PHY_1M);   /* default 1M; MODE/PCNF0 are set by radio_apply_phy */

	NRF_RADIO->PCNF1 =
		((uint32_t)RADIO_PAYLOAD_MAX_LEN << RADIO_PCNF1_MAXLEN_Pos) |
		(0UL << RADIO_PCNF1_STATLEN_Pos) |
		(3UL << RADIO_PCNF1_BALEN_Pos) |
		((uint32_t)RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
		((uint32_t)RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

	NRF_RADIO->CRCCNF =
		((uint32_t)RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
		((uint32_t)RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
	NRF_RADIO->CRCPOLY = 0x0000065BUL;

	NRF_RADIO->RXADDRESSES = 1UL;

	/* nRF54L: the end-of-BLE-reception interrupt is PHYEND; and the interrupt is multi-line, with
	 * INTENSET00 corresponding to IRQ line 0 (RADIO_0_IRQn). ⚠️ Verify on hardware: confirm the INTENSET00↔RADIO_0_IRQn pairing is correct. */
	NRF_RADIO->INTENSET00 = RADIO_INTENSET00_PHYEND_Msk;
}

/*
 * ⚠️ Verify on hardware: the HFXO request. The nRF54L clock subsystem differs from the nRF52.
 * This reuses the nRF52 clock_control path; if mgr is NULL or the request fails on the nRF54L,
 * switch to the nRF54L's corresponding clock-request method (see the zephyr/soc/nordic/nrf54l clock driver).
 */
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
	timestamp_dppi_init();

	/* The nRF54L RADIO has two IRQ lines (RADIO_0/RADIO_1); INTENSET00 goes to line 0 */
	IRQ_CONNECT(RADIO_0_IRQn, RADIO_IRQ_PRIORITY, radio_isr, NULL, 0);
	irq_enable(RADIO_0_IRQn);

	IRQ_CONNECT(SNIFF_TIMER_IRQn, TIMER_IRQ_PRIORITY, timer_isr, NULL, 0);
	irq_enable(SNIFF_TIMER_IRQn);

	return 0;
}

static bool valid_phy(phy_t phy)
{
	return phy == PHY_1M || phy == PHY_2M ||
	       phy == PHY_CODED_S8 || phy == PHY_CODED_S2;
}

int radio_set_phy(phy_t phy)
{
	/* ⚠️ Verify on hardware: nRF54L reception of 2M/Coded is, like the nRF52, unverified for lack of a
	 * controllable peer; but 1M (including connection following) has passed on real hardware. */
	if (!valid_phy(phy)) {
		return -EINVAL;
	}
	primary_phy = phy;
	alt_phy = phy;
	radio_apply_phy(phy);
	return 0;
}

int radio_set_alt_phy(phy_t phy)
{
	if (!valid_phy(phy)) {
		return -EINVAL;
	}
	alt_phy = phy;
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
	/* The nRF54L's DATAWHITE puts the "whitening seed (IV)" and the "whitening polynomial (POLY)" in the same
	 * register. Reset value 0x00890040: POLY=0x89 (exactly the BLE whitening polynomial, preserved via
	 * read-modify-write); the IV reset includes bit6=1 (0x40) —— the leading 1 of the BLE whitening LFSR
	 * initial value. The nRF52's DATAWHITEIV hardwires bit6=1 and only needs the channel number written;
	 * the nRF54L's IV is an explicit 9 bits and must carry bit6 itself, otherwise the whitening initial
	 * value is wrong and payload/CRC are all garbage (observed: dropping bit6 gives CRC_OK=0, all CRC_ERR). */
	NRF_RADIO->DATAWHITE = (NRF_RADIO->DATAWHITE & ~RADIO_DATAWHITE_IV_Msk) |
			       (((uint32_t)ble_channel | 0x40U) << RADIO_DATAWHITE_IV_Pos);
	cur_channel = ble_channel;
	return 0;
}

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

	NRF_RADIO->EVENTS_PHYEND = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;

	/* nRF54L: the end-of-reception event is PHYEND, so the short is PHYEND_DISABLE (rather than the
	 * nRF52's END_DISABLE). READY_START / ADDRESS_RSSISTART have the same names as on the nRF52. */
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_PHYEND_DISABLE_Msk |
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

	rx_running = false;
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_DISABLE = 1;

	while (NRF_RADIO->EVENTS_DISABLED == 0) {
		/* wait for ramp-down */
	}
	NRF_RADIO->EVENTS_DISABLED = 0;
}
