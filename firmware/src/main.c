/*
 * nRF BLE Sniffer — firmware main program
 *
 * Data flow:
 *   RADIO IRQ --(copy & enqueue, non-blocking)--> k_msgq --> sniffer thread --> USB CDC --> Wireshark
 *                                                                         \-> RTT log (for debugging)
 *
 * Why the interrupt doesn't send USB / log directly: both can block for hundreds of µs, enough
 * to make the radio miss back-to-back packets. The interrupt only does a fixed-length copy;
 * everything else is done in the thread.
 *
 * Capture continues as usual when no host is connected, it just isn't sent — this way RTT always
 * shows the radio working.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "radio_hal.h"
#include "host_iface.h"
#include "conn_follower.h"
#include "board_role.h"
#include "tri_coord.h"
#include "sync_line.h"
#include "led_policy.h"
#include "adv_filter.h"
#include "fem_ctrl.h"

LOG_MODULE_REGISTER(sniffer, LOG_LEVEL_INF);

/* Default to capturing on advertising channel 37. On tri-device boards a strap decides which channel to guard (see g_role);
 * for a single board / no strap, fall back to this, and the host can also change it with HOST_CMD_SET_CHANNEL. */
#define DEFAULT_CHANNEL       BLE_CHANNEL_ADV_37

/* This device's role (board_id + guarded advertising channel), obtained by reading the strap at boot */
static struct board_role g_role = { .board_id = 0, .guard_channel = DEFAULT_CHANNEL };

/* Queue depth: enough to absorb USB/logging jitter, without eating too much RAM */
#define CAPTURE_QUEUE_DEPTH   32

#define LED_TICK_MS           20      /* Main loop tick: the LED policy is evaluated once every 20ms */
#define STATS_LOG_INTERVAL_MS 10000

/* On-board LED0 (devicetree alias led0, present on both DK / Dongle) */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* An enqueued packet. pdu is stored fixed-length to avoid variable-length allocation. */
struct captured_packet {
	uint32_t timestamp_us;
	uint32_t access_addr;
	uint32_t crc;
	uint16_t pdu_len;
	int8_t rssi_dbm;
	uint8_t channel;
	uint8_t phy;
	bool crc_ok;
	uint8_t pdu[RADIO_PDU_MAX_LEN];
};

K_MSGQ_DEFINE(capture_q, sizeof(struct captured_packet), CAPTURE_QUEUE_DEPTH, 4);

/* On queue-full, drop the packet and count it — radio timing comes first; never block the interrupt just because the downstream is slow */
static atomic_t queue_drops;

/* Capture statistics. Modified only in the sniffer thread, no lock needed. */
static struct {
	uint32_t crc_ok;
	uint32_t crc_err;
	uint32_t sent;
} stats;

/* Radio-layer per-PHY RX counts (0=1M 1=2M 2/3=Coded), to see whether each PHY decodes healthily */
static uint32_t phy_rx[4];
static uint32_t phy_rx_ok[4];

/* Current capture parameters. The host can change them; the radio interrupt only reads. */
static uint8_t active_channel = DEFAULT_CHANNEL;

/* Advertising filter for single-target mode: the target MAC is set by HOST_CMD_SET_TARGET (CDC interrupt writes,
 * radio interrupt reads; clear active first, then copy, then set — worst case the read side just passes/blocks one extra packet). */
static uint8_t target_mac[6];
static bool target_active;
static uint32_t adv_filtered;   /* Number of advertising packets blocked by the target filter (incremented in the radio interrupt, printed in the stats line) */

/* Set/clear the target MAC (6 little-endian bytes as on air; NULL = clear): applies to both the follower filter and the RX-interrupt advertising filter */
static void apply_target(const uint8_t *mac)
{
	conn_follower_set_target(mac);
	target_active = false;
	if (mac != NULL) {
		memcpy(target_mac, mac, 6);
		target_active = true;
	}
}

/* ---------------------------------------------------------- Advertising PDU parsing */

/* BT Core Spec Vol 6, Part B, §2.3: the advertising PDU type is in the low 4 bits of the LL header byte */
static const char *adv_pdu_type_name(uint8_t header)
{
	static const char *const names[] = {
		"ADV_IND",        "ADV_DIRECT_IND", "ADV_NONCONN_IND", "SCAN_REQ",
		"SCAN_RSP",       "CONNECT_IND",    "ADV_SCAN_IND",    "ADV_EXT_IND",
		"AUX_CONNECT_RSP",
	};
	const uint8_t type = header & 0x0F;

	return (type < ARRAY_SIZE(names)) ? names[type] : "RESERVED";
}

/*
 * Get the offset of AdvA within the payload.
 *
 * Most advertising PDUs' payloads start with AdvA, but SCAN_REQ / CONNECT_IND put the
 * initiator's address first and AdvA after; reading at offset 0 would mislabel ScanA/InitA as AdvA.
 * The ADV_EXT_IND family uses an extended header and has no fixed-position AdvA.
 *
 * @return AdvA offset; a negative value means the PDU has no directly locatable AdvA
 */
static int adv_addr_offset(uint8_t header)
{
	switch (header & 0x0F) {
	case 0x00:  /* ADV_IND */
	case 0x01:  /* ADV_DIRECT_IND */
	case 0x02:  /* ADV_NONCONN_IND */
	case 0x04:  /* SCAN_RSP */
	case 0x06:  /* ADV_SCAN_IND */
		return 0;
	case 0x03:  /* SCAN_REQ: ScanA(6) + AdvA(6) */
	case 0x05:  /* CONNECT_IND: InitA(6) + AdvA(6) */
		return 6;
	default:    /* ADV_EXT_IND / AUX_* : extended header, parsed in a later stage */
		return -1;
	}
}

/* BLE addresses are little-endian on air, so reverse for printing */
static void format_adv_addr(const struct captured_packet *item, char *out, size_t out_sz)
{
	const int off = adv_addr_offset(item->pdu[0]);
	const int payload_len = (int)item->pdu_len - RADIO_PDU_HEADER_LEN;

	if (off < 0 || payload_len < off + 6) {
		strncpy(out, "-", out_sz);
		out[out_sz - 1] = '\0';
		return;
	}

	const uint8_t *a = &item->pdu[RADIO_PDU_HEADER_LEN + off];

	snprintk(out, out_sz, "%02X:%02X:%02X:%02X:%02X:%02X",
		 a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* Short description of a data-channel PDU (classified by LLID) */
static const char *data_pdu_desc(uint8_t header)
{
	switch (header & 0x03) {
	case 0x01:
		return "LL_DATA(empty/cont)";
	case 0x02:
		return "LL_DATA(start)";
	case 0x03:
		return "LL_CONTROL";
	default:
		return "RFU";
	}
}

/* Channels 0..36 are data channels (following a connection); 37/38/39 are advertising channels */
static bool is_adv_channel(uint8_t ch)
{
	return ch >= BLE_CHANNEL_ADV_37;
}

/* ---------------------------------------------------------------- radio */

/* Interrupt context: first feed the connection follower (it may schedule/switch channel based on it), then copy & enqueue for the host */
static void on_radio_packet(const struct radio_packet *pkt)
{
	/* Which link this packet belongs to depends on the AA the radio was tuned to when receiving it */
	const uint32_t frame_aa = radio_get_access_addr();

	/* Single-target mode: once a target MAC is set, under the advertising AA only PDUs related to the target pass on — don't emit a hit,
	 * don't build a slot, don't chase AUX, don't send to the host. Data-channel packets can only come from the followed connection, so let them all through. */
	if (target_active && frame_aa == BLE_ADV_ACCESS_ADDR &&
	    !adv_pdu_matches_target(pkt->pdu, pkt->pdu_len, target_mac)) {
		adv_filtered++;
		return;
	}

	/* Let the tri-device coordination layer see it first (it may emit a SYNC edge / hit notification based on it), then hand to the follower.
	 * Ordering point: coordination must complete the hit broadcast/handoff before conn_follower builds a slot (which asks the claim gate). */
	tri_coord_on_packet(pkt);

	conn_follower_on_packet(pkt);

	struct captured_packet item = {
		.timestamp_us = pkt->timestamp_us,
		.access_addr = frame_aa,
		.crc = pkt->crc,
		.pdu_len = MIN(pkt->pdu_len, RADIO_PDU_MAX_LEN),
		.rssi_dbm = pkt->rssi_dbm,
		.channel = pkt->channel,
		.phy = pkt->phy,
		.crc_ok = pkt->crc_ok,
	};

	memcpy(item.pdu, pkt->pdu, item.pdu_len);

	if (k_msgq_put(&capture_q, &item, K_NO_WAIT) != 0) {
		atomic_inc(&queue_drops);
	}
}

/* Host command. Executed in CDC interrupt context; only does parameter validation and register-level setup. */
static void on_host_command(uint8_t cmd, const uint8_t *args, uint8_t args_len)
{
	switch (cmd) {
	case HOST_CMD_SET_CHANNEL:
		if (args_len < 1 || args[0] > BLE_CHANNEL_MAX) {
			return;   /* Ignore an illegal argument outright, don't change existing state */
		}
		/* Hand to the follower: in scanning state it re-tunes immediately; in following state it is only recorded
		 * and takes effect on return to scanning, without interrupting an ongoing connection follow */
		active_channel = args[0];
		conn_follower_set_scan_channel(args[0]);
		break;

	case HOST_CMD_START:
		(void)radio_rx_start();
		break;

	case HOST_CMD_STOP:
		radio_rx_stop();
		break;

	case HOST_CMD_SET_TARGET:
		if (args_len >= 6) {
			/* All-zero is treated as clearing the filter */
			bool all_zero = true;

			for (int i = 0; i < 6; i++) {
				all_zero = all_zero && (args[i] == 0);
			}
			apply_target(all_zero ? NULL : args);
		}
		break;

	case HOST_CMD_SET_SINGLE_TARGET:
		if (args_len >= 1) {
			conn_follower_set_single_target(args[0] != 0);
		}
		break;

	case HOST_CMD_SET_HOPPING:
		if (args_len >= 1) {
			conn_follower_set_scan_hopping(args[0] != 0);
		}
		break;

	case HOST_CMD_FOLLOW: {
		/* Tri-device co-follow relay: 25-byte little-endian parameters → post into the follower's inject mailbox (the real slot build happens in the radio interrupt). */
		if (args_len < 25) {
			return;
		}
		const uint8_t *a = args;
		struct conn_follow_inject inj;

		inj.aa = (uint32_t)a[0] | ((uint32_t)a[1] << 8) |
			 ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
		inj.crc_init = (uint32_t)a[4] | ((uint32_t)a[5] << 8) | ((uint32_t)a[6] << 16);
		for (int i = 0; i < 5; i++) {
			inj.chan_map[i] = a[7 + i];
		}
		inj.hop = a[12];
		inj.csa2 = a[13];
		inj.interval = (uint16_t)a[14] | ((uint16_t)a[15] << 8);
		inj.latency = (uint16_t)a[16] | ((uint16_t)a[17] << 8);
		inj.timeout = (uint16_t)a[18] | ((uint16_t)a[19] << 8);
		inj.win_size = a[20];
		inj.anchor0_us = (uint32_t)a[21] | ((uint32_t)a[22] << 8) |
				 ((uint32_t)a[23] << 16) | ((uint32_t)a[24] << 24);
		conn_follower_request_inject(&inj);
		break;
	}

	default:
		break;
	}
}

static int sniffer_start(void)
{
	/* FEM before radio: the nRF21540 has no bypass path; with PDN=0 the RF path is broken and the radio receives nothing. */
	int err = fem_ctrl_init();

	if (err != 0) {
		LOG_ERR("fem_ctrl_init failed: %d", err);
		return err;
	}

	err = radio_init(on_radio_packet);
	if (err != 0) {
		LOG_ERR("radio_init failed: %d", err);
		return err;
	}

	err = radio_set_phy(PHY_1M);
	if (err != 0) {
		LOG_ERR("radio_set_phy failed: %d", err);
		return err;
	}

	err = radio_set_channel(active_channel);
	if (err != 0) {
		LOG_ERR("radio_set_channel failed: %d", err);
		return err;
	}

	radio_set_access_addr(BLE_ADV_ACCESS_ADDR);
	radio_set_crcinit(BLE_ADV_CRC_INIT);

	err = radio_rx_start();
	if (err != 0) {
		LOG_ERR("radio_rx_start failed: %d", err);
		return err;
	}

	/* The radio is already scanning; the follower records the scan channel and takes over the subsequent state machine */
	conn_follower_init(active_channel);

	/* Tri-device scheme §5.1: each device stubbornly guards its strap-assigned advertising channel and does **not** round-robin —
	 * this is the prerequisite for three devices to cover 37/38/39 simultaneously and push the CONNECT_IND hit rate to ~100%. When
	 * used as a single board, the host will re-issue HOST_CMD_SET_HOPPING per --scan-hopping to override this default. */
	conn_follower_set_scan_hopping(false);

	/* Tri-device coordination: bring up the SYNC line + inter-board link, register the claim gate (policy per Kconfig, default self-follow). */
	tri_coord_init(g_role.board_id);

	LOG_INF("radio started: board_id=%u guarding channel %u, PHY 1M (auto-follow on CONNECT_IND capture)",
		g_role.board_id, active_channel);

	return 0;
}

/* Capture dequeue thread: sending USB + logging both happen here, never touching interrupt context */
static void sniffer_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct captured_packet item;
	char addr[18];

	while (1) {
		k_msgq_get(&capture_q, &item, K_FOREVER);

		/* Radio-layer per-PHY RX count, distinguishing "a PHY received nothing" from "received but CRC bad" */
		phy_rx[item.phy & 3]++;
		if (item.crc_ok) {
			phy_rx_ok[item.phy & 3]++;
		}

		if (item.crc_ok) {
			stats.crc_ok++;
		} else {
			stats.crc_err++;
		}

		const struct radio_packet pkt = {
			.pdu = item.pdu,
			.pdu_len = item.pdu_len,
			.crc = item.crc,
			.rssi_dbm = item.rssi_dbm,
			.channel = item.channel,
			.crc_ok = item.crc_ok,
			.timestamp_us = item.timestamp_us,
		};

		/* For tri-device merging: carry board_id and the latest SYNC edge tick (sync_epoch),
		 * so the host can align the three timestamp streams on a common time base. */
		if (host_iface_send_packet_tri(&pkt, item.access_addr, item.phy,
					       g_role.board_id, tri_coord_sync_epoch()) == 0) {
			stats.sent++;
		}

		/* Per-packet logging is compiled only at DBG level (elided entirely when the module defaults to INF): in a
		 * 1200-frames/s environment, per-packet printing would saturate the log thread and RTT. To see per-packet output,
		 * change LOG_MODULE_REGISTER to LOG_LEVEL_DBG and rebuild. Only CRC-correct packets are logged; advertising and
		 * data channels interpret the header byte separately. */
		if (item.crc_ok) {
			if (is_adv_channel(item.channel)) {
				format_adv_addr(&item, addr, sizeof(addr));
				LOG_DBG("#%u ch=%u %+d dBm %-15s AdvA=%s len=%u",
					stats.crc_ok, item.channel, item.rssi_dbm,
					adv_pdu_type_name(item.pdu[0]), addr,
					item.pdu_len);
			} else {
				static const char *const pn[] = {"1M","2M","Coded","Coded"};
				LOG_DBG("#%u ch=%u %+d dBm [data/%s] %-14s AA=%08X len=%u",
					stats.crc_ok, item.channel, item.rssi_dbm,
					pn[item.phy & 3],
					data_pdu_desc(item.pdu[0]), item.access_addr,
					item.pdu_len);
			}
		}
	}
}

K_THREAD_DEFINE(sniffer_tid, 2048, sniffer_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(5), 0, 0);

/* ------------------------------------------------------------------- main */

int main(void)
{
	LOG_INF("=== nRF BLE Sniffer starting ===");

	/* Read the strap at boot to decide the role — three devices run identical firmware, each guarding 37/38/39 via strap (scheme §4.5). */
	g_role = board_role_read();
	active_channel = g_role.guard_channel;

	const bool led_ok = gpio_is_ready_dt(&led);

	if (!led_ok) {
		LOG_ERR("LED device not ready");
	} else {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	}

	/* Bring up radio before USB: the radio doesn't depend on USB, and on some platforms (the nRF54L's DesignWare USB)
	 * usb_enable may block when there is no VBUS, so putting it later keeps it from blocking radio bring-up validation. */
	(void)sniffer_start();
	(void)host_iface_init(on_host_command);

	/* Default target at boot (Kconfig, empty = no filter): follow a single target even without a host; once the host connects
	 * HOST_CMD_SET_TARGET overrides it. */
	if (CONFIG_SNIFFER_DEFAULT_TARGET_MAC[0] != '\0') {
		uint8_t mac[6];

		if (adv_filter_parse_mac(CONFIG_SNIFFER_DEFAULT_TARGET_MAC, mac)) {
			apply_target(mac);
			LOG_INF("default target MAC: %s (single-target + advertising filter)", CONFIG_SNIFFER_DEFAULT_TARGET_MAC);
		} else {
			LOG_ERR("default target MAC has invalid format: %s", CONFIG_SNIFFER_DEFAULT_TARGET_MAC);
		}
	}

	int64_t last_stats = k_uptime_get();

	/* LED indication policy (led_policy.h): reflects only this device's own RF following — in scanning state, one short blink per second;
	 * after following a connection, toggles with RX; while following but 500ms without a packet, stays solid on; on supervision-timeout loss, blinks fast 3 times. Unrelated to SYNC. */
	struct led_policy ledp;
	enum led_state led_state_last = LED_STATE_IDLE;
	bool led_level_last = false;

	led_policy_init(&ledp, k_uptime_get_32(), conn_follower_acl_packets(),
			conn_follower_acl_lost_timeouts());

	while (1) {
		const uint32_t now_ms = k_uptime_get_32();
		enum led_state led_state_now;
		const bool led_on = led_policy_step(&ledp, now_ms,
						    conn_follower_active_connections(),
						    conn_follower_acl_packets(),
						    conn_follower_acl_lost_timeouts(),
						    &led_state_now);

		if (led_ok && led_on != led_level_last) {
			led_level_last = led_on;
			gpio_pin_set_dt(&led, led_on ? 1 : 0);
		}

		if (led_state_now != led_state_last) {
			led_state_last = led_state_now;
			LOG_INF("LED: %s", led_state_name(led_state_now));
		}

		/* Tri-device time-base heartbeat (board 0 emits a SYNC edge at ~1Hz; internally rate-limited). */
		tri_coord_periodic();

		if (k_uptime_get() - last_stats >= STATS_LOG_INTERVAL_MS) {
			last_stats = k_uptime_get();

			struct conn_follower_stats cf;

			conn_follower_get_stats(&cf);

			LOG_INF("stats: CRC_OK %u, CRC_ERR %u, sent %u, queue drops %ld, target filtered %u, USB dropped %u",
				stats.crc_ok, stats.crc_err, stats.sent,
				atomic_get(&queue_drops), adv_filtered, host_iface_dropped());

			struct tri_coord_stats tc;

			tri_coord_get_stats(&tc);
			LOG_INF("  tri-device[id=%u]: own hits %u, peer hits %u, handoffs sent %u/recv %u, SYNC emit %u/capture %u, epoch=%u, inter-board tx dropped %u, LED=%s",
				g_role.board_id, tc.own_hits, tc.peer_hits,
				tc.handoffs_sent, tc.handoffs_recv, tc.sync_emits,
				sync_line_capture_count(), tri_coord_sync_epoch(),
				tc.tx_dropped, led_state_name(led_state_last));
			sync_line_log_debug();
			LOG_INF("  multi-target: currently tracking %u (peak %u), CONNECT_IND %u,"
				"event %u, data packets %u, collisions %u, lost %u, PHY switches %u, relay takeovers %u, relocks %u,"
				"ext-adv chases %u (throttled %u), periodic synced %u, BIS %u, CIS %u, subrate %u, PAwR responses %u, AUX connects %u",
				cf.active_now, cf.peak_concurrent, cf.connects_seen,
				cf.conn_events, cf.data_packets, cf.collisions, cf.lost,
				cf.phy_updates, cf.injects_followed, cf.relock_recovered,
				cf.ext_adv_chase, cf.ext_adv_throttled,
				cf.periodic_synced, cf.bis_synced, cf.cis_synced,
				cf.subrate_updates, cf.pawr_responses, cf.aux_connects);
			LOG_INF("  scheduling: max window-open lateness %uus, radio config avg %uus/max %uus (%u times)",
				cf.open_late_max_us,
				cf.open_count ? cf.open_cost_sum_us / cf.open_count : 0,
				cf.open_cost_max_us, cf.open_count);
			LOG_INF("  radio per-PHY RX: 1M %u/%u 2M %u/%u Coded %u/%u (ok/total)",
				phy_rx_ok[0], phy_rx[0], phy_rx_ok[1], phy_rx[1],
				phy_rx_ok[2] + phy_rx_ok[3], phy_rx[2] + phy_rx[3]);
			LOG_INF("  6.x: CS negotiation PDUs %u, frame-space negotiations %u, short connection intervals %u, extended feature sets %u, decision advertising %u",
				cf.cs_pdus, cf.frame_space_updates,
				cf.conn_rate_updates, cf.feature_ext_pdus,
				cf.adv_decision);

			/* Per-connection evaluation record */
			for (uint8_t i = 0; i < CONN_FOLLOW_MAX_SLOTS; i++) {
				struct conn_slot_info si;

				conn_follower_get_slot(i, &si);
				if (si.active) {
					static const char *const phy_name[] = {
						"1M", "2M", "Coded", "Coded",
					};
					/* A short connection interval (6.2) can drop to 375µs; dividing by 1000 would show "0ms",
					 * so intervals <2ms are shown in µs; a negotiated frame space (6.0) is noted alongside. */
					char ivbuf[16];
					char fsbuf[24];

					if (si.interval_us < 2000u) {
						snprintk(ivbuf, sizeof(ivbuf), "%uus",
							 si.interval_us);
					} else {
						snprintk(ivbuf, sizeof(ivbuf), "%ums",
							 si.interval_us / 1000u);
					}
					if (si.frame_space_us != 0u) {
						snprintk(fsbuf, sizeof(fsbuf), " frame_space=%uus",
							 si.frame_space_us);
					} else {
						fsbuf[0] = '\0';
					}
					LOG_INF("    [slot%u] AA=%08X interval=%s PHY=%s "
						"event=%u serves=%u data=%u CRCerr=%u anchor_delta=%dus%s%s",
						i, si.access_addr,
						ivbuf,
						phy_name[si.phy & 0x03], si.event_counter,
						si.serves, si.data_packets, si.crc_errors,
						si.anchor_delta_us,
						fsbuf,
						si.encrypted ? " encrypted" : "");
					if (si.pawr && si.bis_locked) {
						LOG_INF("           PAwR: last periodic event captured "
							"%u/%u subevents subevent_interval=%uus",
							si.bis_ev_hits_last, si.bis_nse,
							si.bis_sub_interval_us);
					} else if (si.pawr) {
						/* Subevent following not enabled (channel-derivation rule still to be solved), following only subevent 0 */
						LOG_INF("           PAwR: %u subevents × %uus,"
							"currently following only subevent 0",
							si.bis_nse,
							si.bis_sub_interval_us);
					} else if (si.bis_nse != 0) {
						/* Last event captured x/NSE subevents — whether "capture all
						 * multiple subevents" is achieved, look right here. */
						LOG_INF("           ISO: BIS#%u/%u last event captured "
							"%u/%u subevents BN=%u subevent_interval=%uus "
							"slot=%uus air=%uus locked=%s missed=%u",
							si.bis_index, si.bis_num,
							si.bis_ev_hits_last, si.bis_nse,
							si.bis_bn,
							si.bis_sub_interval_us,
							si.bis_slot_spacing_us,
							si.bis_pdu_air_us,
							si.bis_locked ? "yes" : "no",
							si.miss_count);
					}
					if (si.subrate_factor > 1) {
						LOG_INF("           subrate: factor=%u cont=%u "
							"supervision_timeout=%ums (received only on subscribed events)",
							si.subrate_factor, si.subrate_cont,
							si.timeout_10ms * 10u);
					}
				}
			}
		}

		k_sleep(K_MSEC(LED_TICK_MS));
	}

	return 0;
}
