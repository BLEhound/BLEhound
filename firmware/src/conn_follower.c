/*
 * Connection Follower implementation — multi-target version (see conn_follower.h for the design notes).
 *
 * A single radio can only be on one channel at a time, so "following multiple connections at once" is
 * essentially **time-division multiplexing**: maintain a set of connection slots, each connection with its
 * own interval / anchor / hop sequence. The scheduler always watches "the earliest upcoming event among all
 * connections", switches the radio over to serve it when due, and after receiving looks at the next earliest
 * one; the gaps between events are used to scan for new CONNECT_INDs.
 *
 * Collisions are unavoidable: when two connections' events overlap in time, the radio can serve only one, and
 * the other is missed this round (record collision + miss, realigning on the next round). The more connections,
 * and the closer their intervals, the harder they collide — this is the physical limit of a single radio, and
 * it is exactly this degradation curve that is being evaluated.
 *
 * All logic runs in interrupt context (RX interrupt + TIMER scheduling interrupt).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "conn_follower.h"
#include "scan_policy.h"
#include "follow_policy.h"
#include "ble_csa.h"
#include "ble_ctrl_pdu.h"   /* Pure parsing of the 6.x new LL control PDUs (has host-side unit tests) */
#include "tri_coord.h"      /* SYNC edge tick in the logs, to cross-check the inter-board anchor conversion */
#include "sync_line.h"

LOG_MODULE_DECLARE(sniffer, LOG_LEVEL_INF);

/* ---- BLE timing constants ---- */
#define UNIT_1_25_MS_US     1250u
#define UNIT_125_US         125u
#define TX_WIN_DELAY_US     1250u
#define AIR_US_PER_BYTE     8u
#define PREAMBLE_AA_US      40u

/* The two units for BIG_Offset in BIGInfo (BT Core Spec 5.2 Vol6 PartB §2.3.4.9) */
#define OFFS_UNIT_30_US     30u
#define OFFS_UNIT_300_US    300u

/* ---- Following-window margins ---- */
#define OPEN_GUARD_BASE_US  300u
#define STEADY_EVENT_US     2600u
#define SUPERVISION_MISS    6u
/* Lower and upper bounds of the loss threshold derived from the real supervision timeout (unit: the number of events we actually listen to).
 * The lower bound prevents deeming loss on a single hiccup when the timeout is very short; the upper bound prevents a 32s timeout + 7.5ms interval from computing thousands of empty waits. */
#define SUPERVISION_MIN     4u
#define SUPERVISION_MAX     400u
/* If a BIS misses this many events in a row, give up the narrow window and fall back to a wide window to re-search the anchor */
#define BIS_RELOCK_MISS     3u
/* M4 connection relock: when a normal connection misses events in a row (before the supervision timeout), widen the window and re-search,
 * covering anchor/PHY offsets and drift/noise losses caused by unparsed updates (encrypted parameter/PHY updates, large WinOffset).
 * RELOCK_AFTER_MISS must be < the lower bound of supervision_limit (SUPERVISION_MIN=4). */
#define RELOCK_AFTER_MISS   2u
/* In unmapped-channel guard mode (encrypted and channel map untrusted) empty events are the norm (no packet
 * when the channel is not in the map), so we must not start widening the window / alternating PHY after 2
 * misses like a normal link; wait for this many consecutive misses before relocking. */
#define UNMAPPED_RELOCK_MISS 8u
#define RELOCK_WIDEN_MAX    5u      /* Max steps of exponential window widening */
#define RELOCK_BASE_SPAN_US 600u    /* Starting extra span for re-search, doubling each step; the total window is still capped at "≤ one interval" */
/* While not anchored, every this-many missed events in a row doubles the search window (up to 5 doublings) */
#define BIS_SEARCH_WIDEN_EVERY 4u
/* Window lead-time for opening an ISO subevent after locking. The anchor has already been calibrated by a real
 * received packet, so we only need to cover the two-sided clock drift within one ISO interval (±500ppm, ~10µs at 10ms)
 * + scheduling interrupt overhead + radio fast ramp-up (~40µs) — we can no longer use the 300µs-class margin of a
 * connection slot, otherwise 2*margin eats the whole sub_interval and the later subevents simply don't fit. */
#define ISO_LOCKED_GUARD_US 120u
/* How long it takes from entering serve_open to the radio actually being able to receive.
 * Measured (per-slot open instrumentation): the CPU time to reconfigure channel/AA/CRC averages 16µs, max 23µs,
 * plus the nRF52 fast ramp-up ~40µs — about 60µs total, round up to 70µs for margin.
 *
 * Key: this cost happens **within the lead time** (the window opens `guard` earlier than the predicted anchor), not
 * between two windows. Earlier it was treated as "switch time to deduct from the schedule budget" and double-counted,
 * squeezing the lead time down to 50µs — shorter than the radio needs to be ready — so every window-open just missed
 * the packet header, showing up as "both subevents were scheduled, yet only one of them was captured". */
#define ISO_RX_READY_US     70u
/* After locking, leave a little extra past the packet tail to cover PDU-length estimation error */
#define ISO_TAIL_GUARD_US   20u
/* A CIS subevent has a C→P and a P→C packet, separated by one T_IFS */
#define T_IFS_US            150u
/* If the scheduled time has already passed, push it back by this much before scheduling — leaving enough time to write the CC
 * register so it takes the normal hardware compare path, rather than falling into radio_sched_at's "manually pend an interrupt" fallback branch every time. */
#define SCHED_LATE_LEAD_US  20u
/* Tri-device co-follow: the injected connection's first window is extra-widened to absorb the inter-board SYNC offset (measured ±54µs) + USB relay jitter. */
#define INJECT_EXTRA_WIN_US 1500u

/* ---- Scanning-state three-channel round-robin ---- */
#define SCAN_DWELL_US       400000u
/* Only bother switching to scan if the gap is ≥ this value; otherwise stay put and wait for the next event (to avoid frequent switching) */
#define SCAN_SLICE_MIN_US   4000u

/* LL control PDU opcodes */
#define LL_CONNECTION_UPDATE_IND 0x00
#define LL_CHANNEL_MAP_IND       0x01
#define LL_TERMINATE_IND         0x02
#define LL_ENC_REQ               0x03
#define LL_PHY_UPDATE_IND        0x18
#define LL_PERIODIC_SYNC_IND     0x1C     /* Periodic sync transfer PAST (5.1): within a connection, pass a periodic
                                          *   advertising SyncInfo to the peer so it can follow without scanning itself */
#define LL_CIS_REQ               0x1F     /* CIS setup: parameters (phy/nse/iso_interval…) */
#define LL_CIS_IND               0x21     /* CIS confirmation: CIS AA + cis_offset + conn_event_count */
#define LL_CIS_TERMINATE_IND     0x25     /* CIS termination (5.2): CIG_ID + CIS_ID + ErrorCode */
#define LL_SUBRATE_REQ           0x26     /* Connection subrate request (5.3), only a request, not yet in effect */
#define LL_SUBRATE_IND           0x27     /* Connection subrate takes effect (5.3): factor/base/latency/cont/timeout */
/* ---- 6.x new LL control PDUs (Core Spec v6.3 Vol6 PartB Table 2.22) ---- */
#define LL_FEATURE_EXT_REQ       0x2B     /* Extended feature set (6.0): feature bitmap expansion */
#define LL_FEATURE_EXT_RSP       0x2C
#define LL_CS_SEC_RSP            0x2D     /* Channel Sounding (6.0) negotiation family, 0x2D..0x3A */
#define LL_CS_CAPABILITIES_REQ   0x2E
#define LL_CS_CAPABILITIES_RSP   0x2F
#define LL_CS_CONFIG_REQ         0x30
#define LL_CS_CONFIG_RSP         0x31
#define LL_CS_REQ                0x32
#define LL_CS_RSP                0x33
#define LL_CS_IND                0x34
#define LL_CS_TERMINATE_REQ      0x35
#define LL_CS_FAE_REQ            0x36
#define LL_CS_FAE_RSP            0x37
#define LL_CS_CHANNEL_MAP_IND    0x38
#define LL_CS_SEC_REQ            0x39
#define LL_CS_TERMINATE_RSP      0x3A
#define LL_FRAME_SPACE_REQ       0x3B     /* Frame-space negotiation (6.0): T_IFS is negotiable */
#define LL_FRAME_SPACE_RSP       0x3C     /* FS(2)+PHYS(1)+Spacing_Types(1) */
#define LL_OTA_UTP_IND           0x3D     /* LE test-mode OTA Unified Test Protocol (6.2) */
#define LL_CONNECTION_RATE_REQ   0x3E     /* Short connection interval (6.2): request, not yet in effect */
#define LL_CONNECTION_RATE_IND   0x3F     /* Short connection interval takes effect: WinOffset/Interval(×125µs)/Instant/... */
/* Contiguous opcode range of the CS negotiation PDUs, convenient for unified counting */
#define LL_CS_OPCODE_MIN         LL_CS_SEC_RSP        /* 0x2D */
#define LL_CS_OPCODE_MAX         LL_CS_TERMINATE_RSP  /* 0x3A */

/* PHY bitmask in LL_PHY_UPDATE_IND: bit0=1M bit1=2M bit2=Coded */
#define PHY_MASK_1M              0x01
#define PHY_MASK_2M              0x02
#define PHY_MASK_CODED           0x04

/* Coded packets are much longer on air (preamble 80µs + AA 256µs, S8 8µs per bit); a typical small packet is ~1.5ms,
 * so a 6ms window covers the vast majority; a huge 255B packet (~17ms) occasionally gets truncated, which is acceptable. */
#define CODED_EVENT_US           6000u

#define ADV_PDU_CONNECT_IND      0x05
#define ADV_PDU_EXT_IND          0x07     /* ADV_EXT_IND / AUX_* share this type */
#define ADV_PDU_DECISION_IND     0x09     /* Decision-based advertising filtering (6.0): new primary-channel PDU type */
#define ADV_HDR_CHSEL2_BIT       0x20

/* Extended advertising (BLE 5.0) AUX-chain following: extended-header flag bits + timing window */
#define EXT_HDR_FLAG_ADVA        0x01
#define EXT_HDR_FLAG_TARGETA     0x02
#define EXT_HDR_FLAG_CTE         0x04
#define EXT_HDR_FLAG_ADI         0x08
#define EXT_HDR_FLAG_AUXPTR      0x10
#define EXT_HDR_FLAG_SYNC        0x20
#define EXT_HDR_FLAG_TXPOWER     0x40
#define AD_TYPE_BIG_INFO         0x2C     /* BIGInfo in ACAD (LE Audio BIS) */
#define AD_TYPE_PAWR_TIMING      0x32     /* PAwR subevent/response-slot timing in ACAD (5.4) */

/*
 * PAwR (5.4) subevent following.
 *
 * The parameters (RspAA / numSubevents / subeventInterval / responseSlotDelay /
 * responseSlotSpacing) are parsed from the ACAD of the AUX_ADV_IND carrying the SyncInfo (AD type 0x32),
 * available right on air, no need to sniff PAST. The time grid = periodic anchor + k×subeventInterval,
 * channel = CSA#2(paEventCounter XOR k) — see the derivation notes in bis_channel_for_subevent().
 *
 * PAWR_CHAN_PROBE is a data-collection switch used when reverse-engineering the channel rule: it sweeps the listening
 * channel of subevents 1..N-1 across all available channels by paEventCounter, printing "PAwR sample <counter> <se> <ch>" on a hit.
 * The rule is already solved, so keep it off normally; turn it on to re-verify in a new environment, together with the fitting tools under tools/pawr/.
 */
#ifndef PAWR_FOLLOW_SUBEVENTS
#define PAWR_FOLLOW_SUBEVENTS 1
#endif
#ifndef PAWR_CHAN_PROBE
#define PAWR_CHAN_PROBE 0
#endif
#define AUX_MAX_OFFSET_US        30000u   /* Don't chase if the aux offset exceeds this (abnormal/too far) */
#define AUX_OPEN_GUARD_US        250u     /* How early to open the RX window */
#define AUX_WINDOW_US            2200u    /* aux RX window (1M/2M) */
#define AUX_WINDOW_CODED_US      6000u    /* Coded aux uses a wider window */
#define AUX_MAX_CHAIN            4u        /* AUX_CHAIN chain depth limit, to prevent runaway */

enum update_kind {
	UPD_CHAN_MAP = 0,
	UPD_CONN,
	UPD_PHY,
	UPD_CONN_RATE,            /* Short connection interval (6.2): interval in units of 125µs, includes subrate */
};

struct pending_update {
	bool valid;
	bool from_hint;           /* From a host key hint (not over-the-air plaintext): the channel map becomes trusted again once the update takes effect */
	uint16_t instant;
	enum update_kind kind;
	uint8_t chan_map[5];
	/* UPD_CONN: the whole LL_CONNECTION_UPDATE_IND (parsing goes through ble_ctrl_pdu, with host unit tests).
	 * Its win_offset determines the new anchor of the instant event — missing it was the root cause of the
	 * 2026-09-19 on-air "always loses tracking when updating back to 48.75ms" bug. */
	struct ble_conn_update_ind conn_update;
	uint16_t latency;         /* UPD_CONN_RATE: new peripheral latency */
	uint16_t timeout_10ms;    /* UPD_CONN_RATE: new supervision timeout, ×10ms */
	uint8_t phy;              /* UPD_PHY: target PHY for the central direction (C→P) */
	uint8_t phy_slave;        /* UPD_PHY: target PHY for the peripheral direction (P→C) */
	/* ---- UPD_CONN_RATE specific (6.2 short connection interval) ---- */
	uint32_t interval_us;     /* Connection interval given directly, µs (= Interval × 125µs) */
	uint32_t win_offset_us;   /* Transmit window offset, µs (= WinOffset × 125µs) */
	uint16_t subrate_factor;  /* Subrate factor after taking effect */
	uint16_t subrate_cont;    /* continuationNumber */
};

/* One tracked connection */
struct conn_slot {
	bool active;
	uint32_t aa;
	uint32_t crc_init;
	uint32_t interval_us;
	uint8_t hop;
	bool csa2;
	uint16_t chan_id;
	uint8_t chan_map[5];
	uint8_t chan_count;
	uint8_t last_unmapped;
	uint16_t event_counter;
	bool encrypted;
	/* Whether the channel map is trusted: after encryption (LL_ENC_REQ) the peer's LL_CHANNEL_MAP_IND is
	 * ciphertext the firmware cannot see, so from then on the map may have been swapped at any time. While
	 * untrusted, every event guards the CSA#2 **unmapped channel** instead: as long as that channel is still
	 * in the peer's map the packet is guaranteed to be there, so a map change cannot lose the link outright
	 * (we merely capture less). The host decrypts the channel-map update with the LTK and relays it (key
	 * hint); once it takes effect at the instant the map is trusted again. */
	bool map_trusted;
	bool periodic;            /* Periodic advertising slot: receives AUX_SYNC_IND, doesn't parse LL control packets */
	bool pawr;                /* PAwR (5.4) periodic train: one periodic event contains multiple subevents */
	uint32_t pawr_rsp_aa;     /* PAwR: access address of the response packet (RspAA in ACAD 0x32) */
	uint32_t pawr_rsp_delay_us;   /* PAwR: subevent start → first response slot */
	uint32_t pawr_rsp_spacing_us; /* PAwR: interval between adjacent response slots */
	bool pawr_in_rsp;         /* PAwR: this service is receiving a response slot, not the subevent itself */
	uint8_t pawr_last_ch;     /* PAwR: the channel used by this subevent — the response slot uses the same channel */
	uint8_t probe_ch;         /* Temporary: for PAwR channel probing */
	uint8_t probe_iso_ch;     /* Temporary: the channel the ISO recurrence would give at the same instant, for comparison */
	bool bis;                 /* ISO slot (BIS/CIS): receives isochronous stream data, doesn't parse LL control packets */
	bool is_cis;              /* This ISO slot is a CIS (matched by CIG_ID/CIS_ID against LL_CIS_TERMINATE_IND) */
	uint8_t cig_id;           /* CIS specific: CIG identifier (from LL_CIS_REQ) */
	uint8_t cis_id;           /* CIS specific: CIS identifier */
	bool bis_locked;          /* ISO: re-anchored on a real ISO packet (a narrow window can be used) */
	uint32_t bis_anchor_us;   /* ISO: fixed time anchor (the **packet start** of subevent 0), counter is computed from it */
	uint16_t bis_anchor_cnt;  /* ISO: the event_counter corresponding to the anchor */
	/* ---- Multi-subevent scheduling within one BIG / CIG event ---- */
	uint8_t bis_nse;          /* Number of subevents of this BIS/CIS per event (NSE) */
	uint8_t bis_bn;           /* Burst Number: number of new payloads per event */
	uint8_t bis_num;          /* Number of BIS streams in this BIG (0 for a CIS slot) */
	uint8_t bis_index;        /* Which BIS this slot follows (1-based; 0 for a CIS slot) */
	uint8_t bis_se_idx;       /* Which subevent is currently being received (0..nse-1) */
	uint8_t bis_ev_hits;      /* Number of CRC-OK subevents captured in this event (accumulated per subevent) */
	uint8_t bis_ev_hits_last; /* Hit count of the last complete event — the "capture all" rate is this/NSE */
	uint32_t bis_sub_interval;/* Interval between adjacent subevents of this stream, µs */
	/* How far the "next occupied instant" on air is from this one, µs; 0 = no more packets later in this event.
	 * For a single stream it's just sub_interval; when following multiple BIS of the same BIG in parallel, under
	 * interleaved layout other BIS are inserted in between (spaced by BIS_Spacing), and the window must be narrowed to the smaller one. */
	uint32_t bis_slot_spacing;
	uint32_t bis_event_us;    /* Anchor of this event (i.e. the packet-start time of subevent 0) */
	uint32_t bis_pdu_air_us;  /* On-air duration within one subevent, used to size the window */
	uint16_t bis_prn_lu;      /* CSA#2 subevent pseudo-random-number recurrence state */
	uint16_t bis_remap_idx;   /* CSA#2 subevent remap-index recurrence state */
	uint8_t phy;              /* Central-direction (C→P) PHY, phy_t */
	uint8_t phy_slave;        /* Peripheral-direction (P→C) PHY; differs from phy iff the link is asymmetric */
	uint32_t next_mts;        /* Predicted time of the next master packet */
	uint32_t first_win_us;    /* Widened window of the first event; 0 means the first event has passed */
	uint16_t miss_count;
	/* ---- Supervision timeout & connection subrating (subrating, BLE 5.3) ---- */
	uint16_t timeout_10ms;    /* Supervision timeout, ×10ms; 0=unknown (fall back to fixed threshold) */
	uint16_t latency;         /* Peripheral latency (number of connection events allowed to be skipped) */
	uint16_t subrate_factor;  /* Subrate factor S; 0/1 = subrating disabled */
	uint16_t subrate_base;    /* Subscribed-event base: on-air activity only when (counter − base) mod S == 0 */
	uint16_t subrate_cont;    /* continuationNumber: after a subscribed event has data, listen this many more events */
	uint16_t cont_left;       /* How many "continuation events" remain to be listened to */
	uint16_t frame_space_us;  /* T_IFS after 6.0 frame-space negotiation, µs; 0=not negotiated (default 150) */
	struct pending_update pending;
	uint32_t events;
	uint32_t serves;          /* Number of times this slot was actually served (RX window opened) */
	uint16_t max_pdu_seen;    /* Longest PDU (bytes) received on this slot, used to size the window to just enough */
	uint32_t data_packets;
	/* ---- Diagnostics: pinpoint which layer "can't capture packets" is stuck at ---- */
	uint32_t crc_errors;      /* Packets where AA matched but CRC failed. >0 means channel and timing are both right,
				   * the problem is CRCInit; ==0 && data_packets==0 means not even AA matched,
				   * the problem is the channel or the anchor timing. */
	int32_t anchor_delta_us;  /* Last RX timestamp − predicted anchor (µs), measures anchor accuracy */
};

/* What the next TIMER schedule should do */
enum sched_action {
	SCHED_NONE = 0,
	SCHED_SERVE,        /* Go serve some connection's event */
	SCHED_SERVE_AUX,    /* Go chase an extended-advertising aux packet */
	SCHED_SCAN_ROTATE,  /* Rotate advertising channels during pure scanning */
};

enum radio_mode {
	MODE_SCANNING = 0,  /* Radio receiving on an advertising channel (pure scanning or scanning during an event gap) */
	MODE_SERVING,       /* Radio serving some connection's event */
	MODE_SERVING_AUX,   /* Radio receiving an extended-advertising AUX packet on a secondary channel */
	MODE_IDLE,          /* Single-target locked: stop receiving during event gaps, don't scan */
};

static struct {
	uint8_t scan_channel;
	bool scan_hopping;
	bool target_active;
	uint8_t target_mac[6];

	/* Extended-advertising AUX-chain following: chase only one chain at a time (register on receiving an AuxPtr, switch to the secondary channel when due) */
	bool aux_pending;
	uint8_t aux_channel;
	uint8_t aux_phy;
	uint32_t aux_at_us;
	uint8_t aux_chain;
	/* Time budget for chasing aux (token bucket, µs). See aux_budget_take(). */
	uint32_t aux_budget_us;
	uint32_t aux_budget_ts;

	struct conn_slot slots[CONN_FOLLOW_MAX_SLOTS];

	enum radio_mode mode;
	int serving_idx;          /* Which slot is being served in MODE_SERVING */
	uint8_t pkts_this_event;
	bool got_master;
	bool got_data;            /* Whether a CRC-OK packet was received this event (a BIS slot uses it to judge liveness) */
	uint32_t master_ts;

	enum sched_action next_action;
	int next_slot;

	struct conn_follower_stats stats;
} f;

/* Two monotonically increasing counters for LED indication (written by ISR, read by thread, 32-bit read/write is atomic):
 * CRC-OK packets received on ACL slots / times an ACL slot was released on supervision timeout. Not in the stats struct, doesn't change the stats-line contract. */
static uint32_t g_acl_packets;
static uint32_t g_acl_lost_timeouts;

/* Single-target mode switch (on by default; HOST_CMD_SET_SINGLE_TARGET can turn it off) */
static bool g_single_target = true;

static void sched_dispatch(void);
static void arm_next(void);
static int build_slot(uint32_t aa, uint32_t crc_init, bool csa2, const uint8_t chan_map[5],
		      uint8_t hop, uint16_t interval, uint16_t latency, uint16_t timeout,
		      uint16_t win_size, uint32_t anchor0_us, uint16_t event_counter,
		      uint32_t extra_win_us, uint8_t phy);
static void serve_close(void);
static void serve_aux_open(void);
static void serve_aux_close(void);
static uint32_t preamble_aa_us(uint8_t phy);
struct sync_params;
static void start_periodic(const struct sync_params *sp, uint32_t carrier_ts);
static bool parse_sync_info_raw(const uint8_t *si, uint8_t phy, struct sync_params *sp);

/* ------------------------------------------------------------ Advertising channels */

/* The 37/38/39 rotation and the "does returning to scan during a gap change channel" decision are in scan_policy.h (host-unit-testable). */

/* Switch the radio to an advertising channel and start scanning (restart RX only when needed) */
static void enter_scan_radio(void)
{
	radio_rx_stop();
	/* Primary advertising channels (37/38/39) use only 1M (2M is not allowed on primary channels); reset PHY when returning to scanning state */
	radio_set_phy(PHY_1M);
	radio_set_channel(f.scan_channel);
	radio_set_access_addr(BLE_ADV_ACCESS_ADDR);
	radio_set_crcinit(BLE_ADV_CRC_INIT);
	radio_rx_start();
	f.mode = MODE_SCANNING;
}

/* ------------------------------------------------------------ Slot management */

static int find_slot_by_aa(uint32_t aa)
{
	for (int i = 0; i < CONN_FOLLOW_MAX_SLOTS; i++) {
		if (f.slots[i].active && f.slots[i].aa == aa) {
			return i;
		}
	}
	return -1;
}

static int alloc_slot(void)
{
	for (int i = 0; i < CONN_FOLLOW_MAX_SLOTS; i++) {
		if (!f.slots[i].active) {
			return i;
		}
	}
	return -1;
}

static uint8_t count_active(void)
{
	uint8_t n = 0;

	for (int i = 0; i < CONN_FOLLOW_MAX_SLOTS; i++) {
		n += f.slots[i].active ? 1 : 0;
	}
	return n;
}

static void refresh_active_stats(void)
{
	f.stats.active_now = count_active();
	if (f.stats.active_now > f.stats.peak_concurrent) {
		f.stats.peak_concurrent = f.stats.active_now;
	}
}

/* ------------------------------------------------------------ External interface */

void conn_follower_init(uint8_t scan_channel)
{
	f.scan_channel = scan_channel;
	f.scan_hopping = true;
	f.mode = MODE_SCANNING;
	/* main has already configured the radio and started rx; here we schedule the first dispatch */
	arm_next();
}

void conn_follower_set_scan_channel(uint8_t ch)
{
	if (ch > BLE_CHANNEL_MAX) {
		return;
	}
	f.scan_channel = ch;
	if (f.mode == MODE_SCANNING) {
		radio_set_channel(ch);
	}
}

void conn_follower_set_scan_hopping(bool enable)
{
	f.scan_hopping = enable;
}

/* Claim gate on a hit (see conn_follower.h). Default NULL = always follow (single-board behavior). */
static conn_claim_gate_t g_claim_gate;

void conn_follower_set_claim_gate(conn_claim_gate_t gate)
{
	g_claim_gate = gate;
}

void conn_follower_set_target(const uint8_t *mac)
{
	if (mac == NULL) {
		f.target_active = false;
		return;
	}
	for (int i = 0; i < 6; i++) {
		f.target_mac[i] = mac[i];
	}
	f.target_active = true;
}

uint8_t conn_follower_last_packet_direction(void)
{
	if (f.mode != MODE_SERVING || f.pkts_this_event == 0) {
		return 0;
	}
	const struct conn_slot *s = &f.slots[f.serving_idx];

	if (s->bis || s->periodic) {
		return 0;
	}
	return f.pkts_this_event == 1 ? 1 : 2;
}

bool conn_follower_is_following(void)
{
	return count_active() > 0;
}

uint8_t conn_follower_active_connections(void)
{
	uint8_t n = 0;

	for (int i = 0; i < CONN_FOLLOW_MAX_SLOTS; i++) {
		const struct conn_slot *s = &f.slots[i];

		if (s->active && !s->periodic && !s->bis) {
			n++;
		}
	}
	return n;
}

uint32_t conn_follower_acl_packets(void)
{
	return g_acl_packets;
}

uint32_t conn_follower_acl_lost_timeouts(void)
{
	return g_acl_lost_timeouts;
}

void conn_follower_set_single_target(bool enable)
{
	g_single_target = enable;
}

/* Single-target mode and already holding at least one ACL connection — at this point don't look at other devices */
static inline bool single_target_locked(void)
{
	return g_single_target && conn_follower_active_connections() > 0;
}

void conn_follower_get_stats(struct conn_follower_stats *out)
{
	f.stats.active_now = count_active();
	*out = f.stats;
}

void conn_follower_get_slot(uint8_t idx, struct conn_slot_info *out)
{
	if (idx >= CONN_FOLLOW_MAX_SLOTS) {
		*out = (struct conn_slot_info){0};
		return;
	}
	const struct conn_slot *s = &f.slots[idx];

	out->active = s->active;
	out->access_addr = s->aa;
	out->interval_us = s->interval_us;
	out->event_counter = s->event_counter;
	out->events = s->events;
	out->serves = s->serves;
	out->data_packets = s->data_packets;
	out->crc_errors = s->crc_errors;
	out->anchor_delta_us = s->anchor_delta_us;
	out->bis_nse = s->bis ? s->bis_nse : 0u;
	out->bis_bn = s->bis_bn;
	out->bis_num = s->bis_num;
	out->bis_index = s->bis_index;
	out->bis_ev_hits_last = s->bis_ev_hits_last;
	out->bis_sub_interval_us = s->bis_sub_interval;
	out->bis_slot_spacing_us = s->bis_slot_spacing;
	out->bis_pdu_air_us = s->bis_pdu_air_us;
	out->bis_locked = s->bis_locked;
	out->pawr = s->pawr;
	out->miss_count = s->miss_count;
	out->subrate_factor = s->subrate_factor;
	out->subrate_cont = s->subrate_cont;
	out->timeout_10ms = s->timeout_10ms;
	out->encrypted = s->encrypted;
	out->phy = s->phy;
	out->frame_space_us = s->frame_space_us;
}

/* ----------------------------------------------- Parse CONNECT_IND → build slot */

/* On-air duration from "AA fully received (timestamp)" to "packet end": header(2)+payload+CRC(3), computed per PHY.
 * Coded: the timestamp reference already includes CI+TERM1 (see preamble_aa_us), the tail = FEC block2 (S×8µs per byte) + TERM2. */
static uint32_t pdu_tail_air_us(uint8_t phy, uint16_t pdu_len)
{
	const uint32_t bytes = (uint32_t)pdu_len + 3u;

	switch (phy) {
	case PHY_2M:
		return bytes * 4u;
	case PHY_CODED_S8:
		return bytes * 64u + 24u;
	case PHY_CODED_S2:
		return bytes * 16u + 6u;
	default:
		return bytes * AIR_US_PER_BYTE;
	}
}

/* CONNECT_IND (primary channel, 1M, txWinDelay 1.25ms) and AUX_CONNECT_REQ (secondary channel, the AuxPtr's PHY,
 * txWinDelay 2.5ms / Coded 3.75ms, Core Vol6 PartB §4.5.3) have the same payload format, so they share this parser;
 * the connection's initial PHY = the PHY the packet was received on (§2.3.3.1: when establishing via a secondary channel, the secondary-channel PHY is retained). */
static void start_following_on(const struct radio_packet *pkt, uint8_t phy,
			       uint32_t tx_win_delay_us, bool force_csa2)
{
	const uint8_t *p = pkt->pdu;
	const uint8_t *ll = &p[2 + 12];   /* header/len(2) + InitA(6) + AdvA(6) */

	const uint32_t aa = (uint32_t)ll[0] | ((uint32_t)ll[1] << 8) |
			    ((uint32_t)ll[2] << 16) | ((uint32_t)ll[3] << 24);

	if (find_slot_by_aa(aa) >= 0) {
		return;   /* Already following this one, ignore the duplicate CONNECT_IND */
	}

	/* Follow policy: no target = observe only; target set = follow only that target (AdvA is at payload offset 6, after InitA). */
	bool adva_matches = f.target_active;

	for (int i = 0; adva_matches && i < 6; i++) {
		adva_matches = p[2 + 6 + i] == f.target_mac[i];
	}
	if (!follow_policy_allows(g_single_target, f.target_active, adva_matches,
				  conn_follower_active_connections())) {
		return;
	}

	/* Tri-device claim gate: whether this device takes on following this connection (default policy A always true; policy B/C or deduplication may veto). */
	if (g_claim_gate != NULL && !g_claim_gate(aa)) {
		return;
	}

	const uint16_t win_size = ll[7];
	const uint16_t win_offset = (uint16_t)ll[8] | ((uint16_t)ll[9] << 8);
	const uint16_t interval = (uint16_t)ll[10] | ((uint16_t)ll[11] << 8);
	const uint16_t latency = (uint16_t)ll[12] | ((uint16_t)ll[13] << 8);
	const uint16_t timeout = (uint16_t)ll[14] | ((uint16_t)ll[15] << 8);
	uint8_t chan_map[5];

	for (int i = 0; i < 5; i++) {
		chan_map[i] = ll[16 + i];
	}
	const uint8_t hop = ll[21] & 0x1F;
	const uint32_t crc_init = (uint32_t)ll[4] | ((uint32_t)ll[5] << 8) | ((uint32_t)ll[6] << 16);
	/* AUX_CONNECT_REQ's ChSel bit is RFU; a connection established via extended advertising **must** use CSA#2
	 * (Core Vol6 PartB §2.3.3.1 / §4.5.8); only a primary-channel CONNECT_IND looks at ChSel. */
	const bool csa2 = force_csa2 || (p[0] & ADV_HDR_CHSEL2_BIT) != 0;

	/* First anchor: connection-setup packet end + txWinDelay + WinOffset */
	const uint32_t anchor0 = pkt->timestamp_us + pdu_tail_air_us(phy, pkt->pdu_len) +
				 tx_win_delay_us + (uint32_t)win_offset * UNIT_1_25_MS_US;

	if (build_slot(aa, crc_init, csa2, chan_map, hop, interval, latency, timeout,
		       win_size, anchor0, 0, 0, phy) >= 0) {
		if (force_csa2) {
			f.stats.aux_connects++;
		}
		LOG_INF("start following AA=%08X %s PHY=%u interval=%uus hop=%u ChSel_bit=%u CSA#%u WinOffset=%u",
			aa, force_csa2 ? "AUX_CONNECT_REQ" : "CONNECT_IND", phy,
			(uint32_t)interval * UNIT_1_25_MS_US, hop,
			(p[0] & ADV_HDR_CHSEL2_BIT) ? 1u : 0u, csa2 ? 2u : 1u, win_offset);
		LOG_INF("  anchor0=%u ts=%u win_size=%u sync_epoch=%u sync_count=%u", anchor0,
			pkt->timestamp_us, win_size, tri_coord_sync_epoch(), sync_line_capture_count());
	}
}

static void start_following(const struct radio_packet *pkt)
{
	start_following_on(pkt, PHY_1M, TX_WIN_DELAY_US, false);
}

/* AUX_CONNECT_REQ via a secondary channel: txWinDelay is 2.5ms (1M/2M) or 3.75ms (Coded) per the secondary-channel PHY. */
static void start_following_aux(const struct radio_packet *pkt)
{
	const bool coded = (pkt->phy == PHY_CODED_S8 || pkt->phy == PHY_CODED_S2);

	start_following_on(pkt, pkt->phy, coded ? 3750u : 2500u, true);
}

/* ---- Common slot-build implementation: shared by start_following (self-capture) and poll_inject (tri-device relay) ----
 * anchor0_us = the anchor of the first event to follow (excluding the preamble); event_counter is the connection-event number for that anchor
 * (0 for self-capture; for relay the host may have already missed the first few events, and poll_inject advances to the first future event and gives k).
 * extra_win_us gives relay injection an extra-widened first window. Returns the slot number; -1 on failure (illegal parameter/already following/slots full). */
static int build_slot(uint32_t aa, uint32_t crc_init, bool csa2, const uint8_t chan_map[5],
		      uint8_t hop, uint16_t interval, uint16_t latency, uint16_t timeout,
		      uint16_t win_size, uint32_t anchor0_us, uint16_t event_counter,
		      uint32_t extra_win_us, uint8_t phy)
{
	const uint8_t chan_count = ble_csa_channel_count(chan_map);

	if (chan_count < 2 || hop < 5 || interval == 0) {
		return -1;
	}
	if (find_slot_by_aa(aa) >= 0) {
		return -1;   /* Already following this one, ignore */
	}

	const int idx = alloc_slot();

	if (idx < 0) {
		return -1;   /* Slots full, can't follow more */
	}

	struct conn_slot *s = &f.slots[idx];

	*s = (struct conn_slot){0};
	s->active = true;
	s->aa = aa;
	s->crc_init = crc_init;
	s->csa2 = csa2;
	s->chan_id = ble_csa_channel_id(aa);
	s->map_trusted = true;
	s->interval_us = (uint32_t)interval * UNIT_1_25_MS_US;
	/* The loss threshold is computed from the link's real supervision timeout, not a fixed number of misses — with large
	 * peripheral latency, or later when subrating is enabled, empty events should be tolerated anyway. */
	s->latency = latency;
	s->timeout_10ms = timeout;
	s->hop = hop;
	s->chan_count = chan_count;
	for (int i = 0; i < 5; i++) {
		s->chan_map[i] = chan_map[i];
	}
	s->event_counter = event_counter;
	s->phy = phy;
	s->phy_slave = phy;
	s->next_mts = anchor0_us + preamble_aa_us(phy);
	s->first_win_us = (uint32_t)win_size * UNIT_1_25_MS_US +
			  2 * OPEN_GUARD_BASE_US + STEADY_EVENT_US + extra_win_us;

	f.stats.connects_seen++;
	refresh_active_stats();

	/* A new connection may be earlier than the currently scheduled dispatch, so reschedule */
	arm_next();
	return idx;
}

/* ---- Tri-device co-follow: the host relay (HOST_CMD_FOLLOW) inject mailbox ----
 * request_inject is called in CDC interrupt context, only copies parameters + sets a flag; the actual slot build happens in the radio interrupt (poll_inject,
 * called from the top of conn_follower_on_packet), to avoid concurrently modifying the slots array / radio schedule with the RX interrupt. */
static struct conn_follow_inject g_inject;   /* Accessed only under irq_lock */
static bool g_inject_pending;

void conn_follower_request_inject(const struct conn_follow_inject *p)
{
	unsigned int key = irq_lock();

	g_inject = *p;
	g_inject_pending = true;
	irq_unlock(key);
}

static void handle_ctrl_pdu(struct conn_slot *s, const uint8_t *ctrl, uint8_t len, bool hinted);

/* ---- Key-hint mailbox: posted from the CDC interrupt, consumed in the radio interrupt (same scheme as inject) ---- */
static struct conn_follow_hint g_hint;   /* accessed only under irq_lock */
static bool g_hint_pending;

void conn_follower_request_hint(const struct conn_follow_hint *h)
{
	unsigned int key = irq_lock();

	g_hint = *h;
	g_hint_pending = true;
	irq_unlock(key);
}

static void poll_hint(void)
{
	if (!g_hint_pending) {
		return;
	}
	unsigned int key = irq_lock();
	struct conn_follow_hint h = g_hint;

	g_hint_pending = false;
	irq_unlock(key);

	if (h.pdu_len < 3 || (h.pdu[0] & 0x03) != 0x03 || h.pdu[1] < 1 ||
	    (uint8_t)(h.pdu[1] + 2u) > h.pdu_len) {
		return;
	}
	for (int i = 0; i < CONN_FOLLOW_MAX_SLOTS; i++) {
		struct conn_slot *s = &f.slots[i];

		if (!s->active || s->bis || s->periodic || s->aa != h.aa) {
			continue;
		}
		handle_ctrl_pdu(s, &h.pdu[2], h.pdu[1], true);
		f.stats.hints_applied++;
		LOG_INF("hint: AA=%08X opcode 0x%02X (instant %u, now %u)", s->aa, h.pdu[2],
			s->pending.valid ? s->pending.instant : 0u, s->event_counter);
		return;
	}
}

static void poll_inject(void)
{
	if (!g_inject_pending) {
		return;
	}

	unsigned int key = irq_lock();
	struct conn_follow_inject p = g_inject;

	g_inject_pending = false;
	irq_unlock(key);

	const uint32_t interval_us = (uint32_t)p.interval * UNIT_1_25_MS_US;

	if (interval_us == 0 || find_slot_by_aa(p.aa) >= 0) {
		return;   /* Illegal, or this device already captured and is following it — ignore the relay */
	}
	if (!follow_policy_allows_inject(g_single_target, f.target_active,
					 conn_follower_active_connections())) {
		return;   /* Single-target mode: no relay without a target, and none while already following another connection */
	}

	/* Advance from anchor0 to "the first event still in the future": when the relay arrives over USB, event0 (or more) may already have passed.
	 * k = ceil((now + lead − anchor0)/interval), event_counter is set to k accordingly, so the CSA hop is correct. */
	const uint32_t now = radio_now_us();
	const uint32_t lead = SCHED_LATE_LEAD_US + PREAMBLE_AA_US;
	const int32_t ahead = (int32_t)(p.anchor0_us - (now + lead));
	uint32_t k = 0;

	if (ahead < 0) {
		k = ((uint32_t)(-ahead) + interval_us - 1u) / interval_us;
	}

	const uint32_t anchor_k = p.anchor0_us + k * interval_us;

	if (build_slot(p.aa, p.crc_init, p.csa2 != 0, p.chan_map, p.hop, p.interval,
		       p.latency, p.timeout, p.win_size, anchor_k, (uint16_t)k,
		       INJECT_EXTRA_WIN_US, PHY_1M) >= 0) {
		f.stats.injects_followed++;
		LOG_INF("inject AA=%08X anchor0=%u now=%u k=%u anchor_k=%u interval=%uus win=%u sync_epoch=%u sync_count=%u",
			p.aa, p.anchor0_us, now, k, anchor_k, interval_us, p.win_size,
			tri_coord_sync_epoch(), sync_line_capture_count());
	}
}

/* ---------------------------------------------------- Serving state: LL control packets */

/* PHY bitmask in LL_PHY_UPDATE_IND → phy_t; returns fallback when the mask is 0 (that direction unchanged) */
/* On-air duration from "packet start (first bit of the preamble)" to "the moment the radio timestamps".
 * Our timestamp comes from RADIO EVENTS_ADDRESS, i.e. the instant the access address finishes being received;
 * subtract this function from the RX timestamp to recover the packet-start time.
 * The values match Zephyr nordic lll_tim_internal.h:addr_us_get()
 * (Coded's 376µs includes preamble 80 + AA 256 + CI 16 + TERM1 24). */
static uint32_t preamble_aa_us(uint8_t phy)
{
	switch (phy) {
	case PHY_2M:
		return 24u;
	case PHY_CODED_S8:
	case PHY_CODED_S2:
		return 376u;
	default:
		return PREAMBLE_AA_US;   /* 1M: preamble 1B + AA 4B = 40µs */
	}
}

/* On-air duration of one ISO PDU (preamble → end of CRC), used to size the RX window.
 * payload uses BIGInfo's Max_PDU (CIS uses LL_CIS_REQ's Max_PDU); an encrypted stream adds 4 bytes of MIC per packet. */
static uint32_t iso_pdu_air_us(uint8_t phy, uint16_t max_pdu, bool encrypted)
{
	const uint32_t body = 2u + max_pdu + (encrypted ? 4u : 0u) + 3u;  /* header+payload+MIC+CRC */

	switch (phy) {
	case PHY_2M:
		return (2u + 4u + body) * 4u;            /* preamble 2B + AA 4B, 4µs per byte */
	case PHY_CODED_S8:
	case PHY_CODED_S2:
		/* FEC block1 (preamble+AA+CI+TERM1) 376µs + S=8 data 64µs per byte + TERM2 24µs */
		return 376u + body * 64u + 24u;
	default:
		return (1u + 4u + body) * 8u;            /* preamble 1B + AA 4B, 8µs per byte */
	}
}

static uint8_t phy_from_mask(uint8_t mask, uint8_t fallback)
{
	if (mask & PHY_MASK_CODED) {
		return PHY_CODED_S8;
	}
	if (mask & PHY_MASK_2M) {
		return PHY_2M;
	}
	if (mask & PHY_MASK_1M) {
		return PHY_1M;
	}
	return fallback;
}

/* ---------------------------------------------- CIS (LE Audio connected isochronous stream, 5.2) */

/* Parameters stashed from LL_CIS_REQ, to build the CIS slot together once LL_CIS_IND arrives */
static struct {
	bool valid;
	uint8_t cig_id;          /* CIG identifier (LL_CIS_TERMINATE_IND matches on it) */
	uint8_t cis_id;          /* CIS identifier */
	uint8_t phy;             /* C→P direction PHY (subevent-0) */
	uint8_t phy_p;           /* P→C direction PHY */
	uint8_t nse;             /* Number of subevents per CIS event */
	uint8_t bn;              /* BN_C_To_P + BN_P_To_C (for display only) */
	uint16_t max_pdu_c;      /* Max_PDU_C_To_P */
	uint16_t max_pdu_p;      /* Max_PDU_P_To_C */
	uint32_t sub_interval;   /* Interval between adjacent subevents, µs */
	uint16_t iso_interval;   /* ×1.25ms */
} cis_pending;

/* Mailbox for LL_PERIODIC_SYNC_IND (PAST, 5.1): handle_data_pdu only stashes the raw SyncInfo and the base event,
 * the actual periodic-slot build happens in poll_past() (in the RX main flow, after start_periodic is defined). */
static struct {
	bool valid;
	uint8_t syncinfo[18];     /* Raw SyncInfo 18 bytes (same format as in the advertising extended header) */
	uint16_t sync_conn_event; /* The base ACL event of the SyncInfo offset (syncConnEventCount) */
	uint8_t phy_mask;         /* Periodic-train PHY bitmask (bit0=1M bit1=2M bit2=Coded) */
	uint32_t acl_anchor_us;   /* The ACL anchor (packet start) when this PDU was received */
	uint32_t acl_interval_us;
	uint16_t acl_event;       /* The ACL event_counter when this PDU was received */
} past_pending;

/* Build a CIS tracking slot using LL_CIS_IND (CIS AA + cis_offset + conn_event_count) + the stashed REQ parameters.
 * Reuses the BIS ISO-slot machinery (csa2 + widened window + time-base counter); the difference is only the parameter source:
 *  - CIS AA comes directly from IND; CRCInit uses the ACL connection's; chan_id=lll_chan_id(CIS AA);
 *  - the channel map uses the ACL connection's; event_counter starts from 0;
 *  - anchor = the anchor of ACL event[conn_event_count] + cis_offset. */
static void start_cis(struct conn_slot *acl, uint32_t cis_aa, uint32_t cis_offset,
		      uint16_t conn_event_count)
{
	if (!cis_pending.valid || cis_pending.iso_interval == 0 ||
	    find_slot_by_aa(cis_aa) >= 0) {
		return;
	}
	const uint8_t chan_count = acl->chan_count;

	if (chan_count < 2) {
		return;
	}
	const int idx = alloc_slot();

	if (idx < 0) {
		return;
	}
	/* CIS starting anchor: advance from the current ACL event (acl->event_counter, anchor≈f.master_ts) to
	 * that conn_event_count event, then add cis_offset.
	 * cis_offset is referenced to the ACL anchor = the **start** of the central's packet, whereas f.master_ts is
	 * the instant AA finished being received — so subtract preamble+AA first, so the ISO-slot anchor is on the same packet-start basis as BIS. */
	int32_t delta = (int32_t)(int16_t)(conn_event_count - acl->event_counter);

	if (delta < 0) {
		delta = 0;
	}
	const uint32_t acl_anchor = f.master_ts - preamble_aa_us(acl->phy);
	const uint32_t cis_anchor = acl_anchor + (uint32_t)delta * acl->interval_us + cis_offset;

	struct conn_slot *c = &f.slots[idx];

	*c = (struct conn_slot){0};
	c->active = true;
	c->bis = true;                     /* Reuse the ISO-slot machinery */
	c->is_cis = true;
	c->cig_id = cis_pending.cig_id;
	c->cis_id = cis_pending.cis_id;
	c->aa = cis_aa;
	c->crc_init = acl->crc_init;        /* CIS uses the ACL connection's CRCInit */
	c->csa2 = true;
	c->chan_id = ble_csa_channel_id(cis_aa);
	c->interval_us = (uint32_t)cis_pending.iso_interval * UNIT_1_25_MS_US;
	c->chan_count = chan_count;
	for (int i = 0; i < 5; i++) {
		c->chan_map[i] = acl->chan_map[i];
	}
	c->event_counter = 0;               /* cisEventCount starts from 0 */
	c->phy = cis_pending.phy;
	c->phy_slave = cis_pending.phy_p;
	c->next_mts = cis_anchor;
	c->first_win_us = 4 * OPEN_GUARD_BASE_US + STEADY_EVENT_US + 2000u;
	c->bis_anchor_us = cis_anchor;
	c->bis_anchor_cnt = 0;
	c->bis_event_us = cis_anchor;
	/* Multi-subevent: sub_interval == 0 (NSE=1) degenerates to receiving only subevent 0 */
	c->bis_nse = (cis_pending.sub_interval > 0u && cis_pending.nse > 0u)
			     ? cis_pending.nse : 1u;
	c->bis_bn = cis_pending.bn;
	c->bis_sub_interval = cis_pending.sub_interval;
	/* A CIS slot follows only one stream, so the window upper bound is constrained only by the next subevent of the same stream */
	c->bis_slot_spacing = (c->bis_nse > 1u) ? cis_pending.sub_interval : 0u;
	/* One CIS subevent is "C→P packet + T_IFS + P→C packet"; the window must cover both directions */
	c->bis_pdu_air_us = iso_pdu_air_us(cis_pending.phy, cis_pending.max_pdu_c,
					   acl->encrypted) +
			    T_IFS_US +
			    iso_pdu_air_us(cis_pending.phy_p, cis_pending.max_pdu_p,
					   acl->encrypted);

	cis_pending.valid = false;
	f.stats.cis_synced++;
	refresh_active_stats();
}

static void handle_ctrl_pdu(struct conn_slot *s, const uint8_t *ctrl, uint8_t len, bool hinted);

static void handle_data_pdu(struct conn_slot *s, const struct radio_packet *pkt)
{
	const uint8_t llid = pkt->pdu[0] & 0x03;
	const uint8_t len = pkt->pdu[1];

	if (llid != 0x03 || len < 1) {
		return;
	}

	const uint8_t *ctrl = &pkt->pdu[2];
	const uint8_t opcode = ctrl[0];

	if (opcode == LL_ENC_REQ) {
		s->encrypted = true;   /* Payload is ciphertext afterwards, no longer parsed */
		s->map_trusted = false; /* Channel-map updates are invisible from here on: guard the unmapped channel and wait for a host hint */
		return;
	}
	if (s->encrypted) {
		return;
	}
	handle_ctrl_pdu(s, ctrl, len, false);
}

/* Plaintext LL control PDU: from the air (hinted=false) or from a host key hint (hinted=true, relayed after decryption). */
static void handle_ctrl_pdu(struct conn_slot *s, const uint8_t *ctrl, uint8_t len, bool hinted)
{
	const uint8_t opcode = ctrl[0];

	switch (opcode) {
	case LL_CHANNEL_MAP_IND:
		if (len >= 8) {
			s->pending.valid = true;
			s->pending.from_hint = hinted;
			s->pending.kind = UPD_CHAN_MAP;
			s->pending.instant = (uint16_t)ctrl[6] | ((uint16_t)ctrl[7] << 8);
			for (int i = 0; i < 5; i++) {
				s->pending.chan_map[i] = ctrl[1 + i];
			}
		}
		break;
	case LL_CONNECTION_UPDATE_IND:
		/* Field parsing (including WinOffset and Interval/WinOffset range checks) goes through a pure function with host unit tests. */
		{
			struct ble_conn_update_ind cu;

			if (ble_parse_conn_update_ind(ctrl, len, &cu)) {
				s->pending.valid = true;
				s->pending.from_hint = hinted;
				s->pending.kind = UPD_CONN;
				s->pending.conn_update = cu;
				s->pending.instant = cu.instant;
			}
		}
		break;
	case LL_PHY_UPDATE_IND:
		/* opcode(1) + M_TO_S_PHY(1) + S_TO_M_PHY(1) + Instant(2).
		 * The two directions may differ (asymmetric link, e.g. C→P Coded, P→C 1M) — record each separately,
		 * and at the instant let the radio alternate packet by packet within the event. A mask of 0 means that direction is unchanged. */
		if (len >= 5) {
			s->pending.valid = true;
			s->pending.from_hint = hinted;
			s->pending.kind = UPD_PHY;
			s->pending.phy = phy_from_mask(ctrl[1], s->phy);
			s->pending.phy_slave = phy_from_mask(ctrl[2], s->phy_slave);
			s->pending.instant = (uint16_t)ctrl[3] | ((uint16_t)ctrl[4] << 8);
		}
		break;
	case LL_CIS_REQ:
		/* Stash the parameters needed to build the CIS slot, to be used together once LL_CIS_IND arrives.
		 * Field offsets (BT Core Spec 5.2 Vol6 PartB §2.4.2.29, ctrl[0] is the opcode):
		 *   [3]PHY_C_To_P [4]PHY_P_To_C [15..16]Max_PDU_C_To_P [17..18]Max_PDU_P_To_C
		 *   [19]NSE [20..22]Sub_Interval [23]BN [26..27]ISO_Interval */
		if (len >= 28) {
			cis_pending.valid = true;
			cis_pending.cig_id = ctrl[1];
			cis_pending.cis_id = ctrl[2];
			cis_pending.phy = phy_from_mask(ctrl[3], PHY_1M);
			cis_pending.phy_p = phy_from_mask(ctrl[4], cis_pending.phy);
			cis_pending.max_pdu_c = (uint16_t)ctrl[15] | ((uint16_t)ctrl[16] << 8);
			cis_pending.max_pdu_p = (uint16_t)ctrl[17] | ((uint16_t)ctrl[18] << 8);
			cis_pending.nse = ctrl[19];
			cis_pending.sub_interval = (uint32_t)ctrl[20] |
						   ((uint32_t)ctrl[21] << 8) |
						   ((uint32_t)ctrl[22] << 16);
			cis_pending.bn = ctrl[23];
			cis_pending.iso_interval = (uint16_t)ctrl[26] |
						   ((uint16_t)ctrl[27] << 8);
		}
		break;
	case LL_CIS_IND:
		/* CIS AA(4) + cis_offset(3) + … + conn_event_count(2)@offset14 */
		if (len >= 16) {
			const uint32_t cis_aa = (uint32_t)ctrl[1] |
						((uint32_t)ctrl[2] << 8) |
						((uint32_t)ctrl[3] << 16) |
						((uint32_t)ctrl[4] << 24);
			const uint32_t cis_off = (uint32_t)ctrl[5] |
						 ((uint32_t)ctrl[6] << 8) |
						 ((uint32_t)ctrl[7] << 16);
			const uint16_t cec = (uint16_t)ctrl[14] |
					     ((uint16_t)ctrl[15] << 8);

			start_cis(s, cis_aa, cis_off, cec);
		}
		break;
	case LL_CIS_TERMINATE_IND:
		/* CIS end: find the corresponding ISO slot by CIG_ID+CIS_ID and release it immediately, no need to wait for supervision timeout.
		 * CtrData: CIG_ID(ctrl[1]) CIS_ID(ctrl[2]) ErrorCode(ctrl[3]). */
		if (len >= 4) {
			for (int i = 0; i < CONN_FOLLOW_MAX_SLOTS; i++) {
				struct conn_slot *cs = &f.slots[i];

				if (cs->active && cs->is_cis &&
				    cs->cig_id == ctrl[1] && cs->cis_id == ctrl[2]) {
					cs->active = false;
					f.stats.lost++;
					refresh_active_stats();
					LOG_INF("CIS terminated AA=%08X CIG=%u CIS=%u",
						cs->aa, ctrl[1], ctrl[2]);
				}
			}
		}
		break;
	case LL_PERIODIC_SYNC_IND:
		/* PAST (5.1): stash the periodic advertising SyncInfo; the actual slot build happens in poll_past().
		 * CtrData: ID(2) SyncInfo(18) connEventCount(2) lastPACounter(2)
		 *   SID/AType/SCA(1) PHY(1) AdvA(6) syncConnEventCount(2) = 34 bytes.
		 * ctrl[0]=opcode, so ctrl[3..20]=SyncInfo, ctrl[26]=PHY, ctrl[33..34]=syncConnEventCount. */
		if (len >= 35) {
			past_pending.valid = true;
			for (int i = 0; i < 18; i++) {
				past_pending.syncinfo[i] = ctrl[3 + i];
			}
			past_pending.sync_conn_event =
				(uint16_t)ctrl[33] | ((uint16_t)ctrl[34] << 8);
			past_pending.phy_mask = ctrl[26];
			past_pending.acl_anchor_us = f.master_ts - preamble_aa_us(s->phy);
			past_pending.acl_interval_us = s->interval_us;
			past_pending.acl_event = s->event_counter;
		}
		break;
	case LL_SUBRATE_IND:
		/* Connection subrate takes effect (5.3), sent one-way by the central, no instant — takes effect on receipt.
		 * subrateFactor(2) subrateBaseEvent(2) latency(2) continuationNumber(2)
		 * timeout(2), all little-endian. After taking effect there is on-air activity only on subscribed events/continuation events,
		 * and subrate_skip() skips the rest accordingly. */
		if (len >= 11) {
			const uint16_t factor = (uint16_t)ctrl[1] | ((uint16_t)ctrl[2] << 8);

			if (factor >= 1u) {
				s->subrate_factor = factor;
				s->subrate_base = (uint16_t)ctrl[3] | ((uint16_t)ctrl[4] << 8);
				s->latency = (uint16_t)ctrl[5] | ((uint16_t)ctrl[6] << 8);
				s->subrate_cont = (uint16_t)ctrl[7] | ((uint16_t)ctrl[8] << 8);
				s->timeout_10ms = (uint16_t)ctrl[9] | ((uint16_t)ctrl[10] << 8);
				/* When it has just taken effect, treat it as "still within the continuation window", to avoid landing exactly
				 * on a non-subscribed event, skipping straight through, and missing the rest of this round's packets. */
				s->cont_left = s->subrate_cont;
				f.stats.subrate_updates++;
			}
		}
		break;
	case LL_SUBRATE_REQ:
		/* Only a request (the peripheral may also send it); actual effect follows LL_SUBRATE_IND, don't touch parameters here */
		break;
	case LL_CONNECTION_RATE_IND:
		/* Short connection interval takes effect (6.2): interval in units of **125µs** (not the old 1.25ms),
		 * integrates subrate/latency/timeout, carries an Instant. CtrData (14 bytes, all little-endian):
		 *   WinOffset(2)×125µs Interval(2)×125µs Instant(2) SubrateFactor(2)
		 *   Latency(2) ContinuationNumber(2) Timeout(2)×10ms
		 * This is the only entry point for following a sub-7.5ms connection — CONNECT_IND's interval is still in 1.25ms units,
		 * minimum 7.5ms; dropping down to 375µs is done by switching over via this PDU. */
		{
			/* Field parsing goes through a pure function with host-side unit tests (including ECV range checks, rejecting the
			 * misdetection of random llid=3 bytes hitting 0x3F in an unencrypted connection). */
			struct ble_conn_rate_ind cr;

			if (ble_parse_conn_rate_ind(ctrl, len, &cr)) {
				s->pending.valid = true;
				s->pending.kind = UPD_CONN_RATE;
				s->pending.win_offset_us = cr.win_offset_us;
				s->pending.interval_us = cr.interval_us;
				s->pending.instant = cr.instant;
				s->pending.subrate_factor = cr.subrate_factor;
				s->pending.latency = cr.latency;
				s->pending.subrate_cont = cr.continuation_number;
				s->pending.timeout_10ms = cr.timeout_10ms;
			}
		}
		break;
	case LL_CONNECTION_RATE_REQ:
		/* Only a request (parameter negotiation); actual effect follows LL_CONNECTION_RATE_IND */
		break;
	case LL_FRAME_SPACE_RSP:
		/* Frame-space negotiation result (6.0): FS(2)=the chosen frame space µs, PHYS(1), Spacing_Types(1).
		 * bit0/bit1 = T_IFS_ACL_CP / T_IFS_ACL_PC — on a hit, update this link's effective T_IFS.
		 * A short connection interval requires squeezing the frame space to the minimum, so the two usually appear together. */
		{
			/* Pure-function parsing + 10ms upper-bound check (rejects the misdetection of random llid=3 bytes hitting 0x3C) */
			struct ble_frame_space_rsp fsr;

			if (ble_parse_frame_space_rsp(ctrl, len, &fsr)) {
				if (fsr.spacing_types & BLE_FS_SPACING_ACL_MASK) {
					s->frame_space_us = fsr.fs_us;
				}
				f.stats.frame_space_updates++;
			}
		}
		break;
	case LL_FRAME_SPACE_REQ:
		/* Only a request; the chosen value follows LL_FRAME_SPACE_RSP */
		break;
	case LL_FEATURE_EXT_REQ:
	case LL_FEATURE_EXT_RSP:
		/* Extended feature set (6.0): feature bitmap expansion. Doesn't affect following, counted for logs/Wireshark */
		f.stats.feature_ext_pdus++;
		break;
	case LL_OTA_UTP_IND:
		/* LE test-mode OTA Unified Test Protocol (6.2). Carries UTP data, usually doesn't appear while following */
		break;
	case LL_TERMINATE_IND:
		s->active = false;
		f.stats.lost++;
		refresh_active_stats();
		break;
	default:
		/* Channel Sounding negotiation family (6.0, opcodes 0x2D..0x3A): the sniffer can only passively see
		 * these negotiation PDUs (capabilities/config/start/terminate) exchanged in the clear on the ACL; the actual phase-ranging
		 * tones are on the CS physical channel + LE 2M 2BT PHY, and their phase is relative to each side's own local oscillator, so a third party
		 * cannot passively reconstruct them into a distance — see 6.17 "the honest limits of CS capture". Counted uniformly here for the logs. */
		if (opcode >= LL_CS_OPCODE_MIN && opcode <= LL_CS_OPCODE_MAX) {
			f.stats.cs_pdus++;
		}
		break;
	}
}

static void apply_pending_if_due(struct conn_slot *s)
{
	/* Use "has the instant been reached/passed" rather than exact equality: after subrating takes effect we batch-skip
	 * connection events with no on-air activity, and exact equality could miss the instant entirely, so the update would never take effect. */
	if (!s->pending.valid ||
	    (int16_t)(s->event_counter - s->pending.instant) < 0) {
		return;
	}

	switch (s->pending.kind) {
	case UPD_CONN: {
		const struct ble_conn_update_ind *u = &s->pending.conn_update;

		const uint32_t old_interval_us = s->interval_us;
		/* A key hint may arrive only after the instant (host decryption/relay latency): next_mts is then the
		 * value extrapolated from the instant by `late` more events at the old interval, whereas the real
		 * anchor has run at the new interval since the instant, so the interval difference for those `late`
		 * events must be added back. On the over-the-air plaintext path `late` is always 0. */
		const uint16_t late = (uint16_t)(s->event_counter - s->pending.instant);

		s->interval_us = (uint32_t)u->interval * UNIT_1_25_MS_US;
		s->latency = u->latency;
		s->timeout_10ms = u->timeout_10ms;
		/* The master's first packet at the instant event is not at the old anchor, but in a WinSize-wide transmit window starting at
		 * "old anchor + WinOffset" (Core Vol6 PartB §5.1.1). next_mts is right now the old anchor extrapolated to the instant by the old
		 * interval (serve_close computes next_mts before calling this function), so shift it here; the first window is still widened by WinSize
		 * to cover any position within the window. On air: WinOffset=32 (40ms) always loses tracking if not shifted. */
		s->next_mts = ble_conn_update_new_anchor_us(s->next_mts, u) +
			      (uint32_t)late * (s->interval_us - old_interval_us);
		if (late != 0u) {
			LOG_INF("conn update applied %u events late (interval %u -> %u us)", late,
				old_interval_us, s->interval_us);
		}
		s->first_win_us = (uint32_t)u->win_size * UNIT_1_25_MS_US +
				  2 * OPEN_GUARD_BASE_US + STEADY_EVENT_US;
		f.stats.conn_updates++;
		break;
	}
	case UPD_PHY:
		/* From this event on, the master/slave directions each send on the new PHY; if asymmetric, the radio alternates
		 * packet by packet within the event (see serve_open). After a PHY switch the master's packet position on air changes, so use a widened
		 * "re-capture" window to cover re-locking within this event. */
		s->phy = s->pending.phy;
		s->phy_slave = s->pending.phy_slave;
		s->first_win_us = 4 * OPEN_GUARD_BASE_US + CODED_EVENT_US;
		f.stats.phy_updates++;
		break;
	case UPD_CONN_RATE:
		/* Short connection interval takes effect (6.2): the interval is given directly in µs (already ×125 at parse time). The interval can drop to
		 * 375µs — the steady-state window/lead time are all adaptively narrowed by serve_open()/open_guard() per the new interval.
		 * When the integrated subrate factor is >1 it takes effect together (base taken as the instant event). */
		s->interval_us = s->pending.interval_us;
		s->latency = s->pending.latency;
		s->timeout_10ms = s->pending.timeout_10ms;
		/* Same as UPD_CONN: the new anchor of the instant event = old anchor + WinOffset (§2.4.2.58). */
		s->next_mts += s->pending.win_offset_us;
		if (s->pending.subrate_factor > 1u) {
			s->subrate_factor = s->pending.subrate_factor;
			s->subrate_base = s->pending.instant;
			s->subrate_cont = s->pending.subrate_cont;
			s->cont_left = s->pending.subrate_cont;
		}
		/* The interval changed and the anchor will shift: use a widened window not exceeding the new interval to re-lock in this event. */
		s->first_win_us = 2 * OPEN_GUARD_BASE_US + STEADY_EVENT_US;
		if (s->interval_us > 0u && s->first_win_us > s->interval_us) {
			s->first_win_us = s->interval_us;
		}
		f.stats.conn_rate_updates++;
		break;
	case UPD_CHAN_MAP:
	default:
		for (int i = 0; i < 5; i++) {
			s->chan_map[i] = s->pending.chan_map[i];
		}
		s->chan_count = ble_csa_channel_count(s->chan_map);
		f.stats.map_updates++;
		if (s->pending.from_hint) {
			s->map_trusted = true;   /* The new map the host decrypted is the one the peer uses from the instant on */
		}
		break;
	}
	s->pending.valid = false;
}

/* Connection subrating (subrating, BLE 5.3): advance event_counter / next_mts to the next connection event that
 * actually has on-air activity.
 *
 * After subrating is enabled, the peripheral only listens on "subscribed events":
 *     (connEventCounter − subrateBaseEvent) mod subrateFactor == 0
 * plus continuationNumber "continuation events" after receiving a packet with data on a subscribed event.
 * On all other events neither side transmits/receives — listening there not only wastes the radio (other slots are waiting), but also gets
 * counted as a miss, and a slightly large subrate factor would falsely deem loss.
 *
 * The only thing that can't be skipped is a "scheduled update instant": skipping it would make the anchor/channel map/PHY all mismatch,
 * so on encountering a pending instant, stop right on it. */
static void subrate_skip(struct conn_slot *s)
{
	if (s->subrate_factor <= 1u) {
		return;
	}
	if (s->cont_left > 0) {
		s->cont_left--;
		return;   /* Continuation event, listen anyway */
	}

	const uint16_t phase = (uint16_t)((uint16_t)(s->event_counter - s->subrate_base) %
					  s->subrate_factor);

	if (phase == 0) {
		return;   /* It's a subscribed event to begin with */
	}

	uint16_t skip = (uint16_t)(s->subrate_factor - phase);

	if (s->pending.valid) {
		const int16_t to_instant = (int16_t)(s->pending.instant - s->event_counter);

		if (to_instant >= 0 && (uint16_t)to_instant < skip) {
			skip = (uint16_t)to_instant;
		}
	}
	if (skip == 0) {
		return;
	}

	s->event_counter = (uint16_t)(s->event_counter + skip);
	s->next_mts += (uint32_t)skip * s->interval_us;
}

/* The channel of an ISO slot's current subevent. Within an event, subevent 0 kicks off with the event-level algorithm,
 * then each subsequent subevent walks the same recurrence state (prn_lu / remap_idx) forward — the order can't be skipped. */
static uint8_t bis_channel_for_subevent(struct conn_slot *s)
{
	if (s->pawr) {
		/* PAwR (5.4): the channel of subevent k = CSA#2(paEventCounter XOR k).
		 *
		 * Not the BIS "event kickoff + subevent recurrence", but each subevent runs the event-level CSA#2 afresh,
		 * only replacing counter with paEventCounter XOR the subevent index.
		 * At k=0 this degenerates to the plain periodic-advertising csa2(paEventCounter), consistent with 5.0 behavior.
		 *
		 * This repo's Zephyr controller has no PAwR on-air implementation, so there is no authoritative code to compare against;
		 * this rule was reverse-engineered and verified on real hardware — sweeping the listening channels of subevents 1..4 across all
		 * available channels by paEventCounter gave 222 ground-truth (counter, subevent, channel) tuples, and this formula matches 222/222.
		 * The deciding case is subevent 3: the counter offset that explains it shows both ±1 and ±3 at once, which no additive rule can
		 * produce, whereas XOR gives exactly this (the low two bits decide +3/+1/−1/−3). See tools/pawr/ for the data-collection and fitting tools. */
		return ble_csa2_next((uint16_t)(s->event_counter ^ s->bis_se_idx),
				     s->chan_id, s->chan_map, s->chan_count);
	}

	if (s->bis_se_idx == 0) {
		return ble_csa2_iso_event(s->event_counter, s->chan_id, s->chan_map,
					  s->chan_count, &s->bis_prn_lu,
					  &s->bis_remap_idx);
	}

	const uint8_t iso_ch = ble_csa2_iso_subevent(s->chan_id, s->chan_map,
						     s->chan_count, &s->bis_prn_lu,
						     &s->bis_remap_idx);

#if PAWR_CHAN_PROBE
	/* Temporary probe: sweep the listening channels of PAwR subevents 1..N-1 across all available channels by paEventCounter,
	 * printing (counter, se, channel) on a hit — to reverse-engineer PAwR's channel-derivation rule from measured ground truth.
	 * Each subevent uses a different phase (4*cnt + se-1) so they don't collide, and one sweep can capture N-1 samples at once. */
	if (s->pawr) {
		uint8_t want = (uint8_t)(((uint32_t)s->event_counter * 4u +
					  s->bis_se_idx - 1u) % s->chan_count);
		uint8_t idx = 0;

		for (uint8_t ch = 0; ch < 37; ch++) {
			if (!(s->chan_map[ch >> 3] & (1u << (ch & 7)))) {
				continue;
			}
			if (idx == want) {
				s->probe_ch = ch;
				s->probe_iso_ch = iso_ch;
				return ch;
			}
			idx++;
		}
	}
#endif
	return iso_ch;
}

/* Channel map with all 37 data channels enabled: what CSA#2 computes on it is the "unmapped channel" (no remapping needed). */
static const uint8_t k_full_chan_map[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};

/* Whether we are in unmapped-channel guard mode: encrypted with an untrusted channel map (and no key hint has fixed it yet). */
static inline bool unmapped_mode(const struct conn_slot *s)
{
	return s->csa2 && s->encrypted && !s->map_trusted && !s->bis && !s->periodic;
}

/* Relock threshold: while guarding the unmapped channel empty events are the norm, so the threshold is much higher. */
static inline uint16_t relock_after(const struct conn_slot *s)
{
	return unmapped_mode(s) ? UNMAPPED_RELOCK_MISS : RELOCK_AFTER_MISS;
}

static uint8_t channel_for_event(struct conn_slot *s)
{
	if (unmapped_mode(s)) {
		f.stats.unmapped_events++;
		return ble_csa2_next(s->event_counter, s->chan_id, k_full_chan_map, 37u);
	}
	if (s->csa2) {
		return ble_csa2_next(s->event_counter, s->chan_id,
				     s->chan_map, s->chan_count);
	}
	return ble_csa1_next(&s->last_unmapped, s->hop, 0, s->chan_map, s->chan_count);
}

/* ------------------------------------------- Time budget for extended-advertising tracking (throttling) */

/*
 * Why throttle: in a busy environment (measured in an office), there are tens of ADV_EXT_INDs carrying AuxPtr per second,
 * and each chase means leaving the advertising channel to dwell 2.2ms (6ms for Coded) on the secondary channel. If we chased them all,
 * the radio would spend the vast majority of its time off 37/38/39, and **a one-shot CONNECT_IND / LL_CIS_IND would be missed** —
 * measured, it took 6 reconnects to capture one CONNECT_IND.
 *
 * Use a token bucket to keep "chasing aux" within a time fraction: refill at the AUX_DUTY ratio per real elapsed time,
 * and deduct one window's duration per chase. When the bucket is empty, don't chase, leaving the radio for scanning and connection following.
 * Charge/decide only at the **chain head** (see arm_next): an AUX_CHAIN_IND already being chased must be finished,
 * since giving up halfway wastes the time already spent.
 */
#define AUX_DUTY_NUM        3u       /* Spend at most 30% of the time chasing aux */
#define AUX_DUTY_DEN        10u
#define AUX_BUDGET_MAX_US   20000u   /* Stop accumulating once 20ms is banked (allows short bursts) */

static bool aux_budget_take(uint32_t now)
{
	/* Refill tokens by the real elapsed time */
	const uint32_t elapsed = now - f.aux_budget_ts;

	f.aux_budget_ts = now;
	f.aux_budget_us += (elapsed / AUX_DUTY_DEN) * AUX_DUTY_NUM;
	if (f.aux_budget_us > AUX_BUDGET_MAX_US) {
		f.aux_budget_us = AUX_BUDGET_MAX_US;
	}

	if (f.aux_chain > 0) {
		return true;   /* Mid-chain: already invested, finish it first */
	}

	const uint32_t cost = AUX_OPEN_GUARD_US +
			      ((f.aux_phy == PHY_CODED_S8 || f.aux_phy == PHY_CODED_S2)
				       ? AUX_WINDOW_CODED_US : AUX_WINDOW_US);

	if (f.aux_budget_us < cost) {
		return false;
	}
	f.aux_budget_us -= cost;
	return true;
}

/* ------------------------------------------------------ Scheduling (multi-target core) */

/* How much earlier than the predicted anchor the RX window opens.
 * A locked ISO slot uses a very small margin (the anchor is calibrated by a real received packet on every subevent); the rest use a
 * drift estimate of "base margin + 1µs per 1ms of interval". earliest_event() scheduling and serve_open() window sizing must use the same value,
 * otherwise the window would be computed off by a chunk. */
static uint32_t open_guard_us(const struct conn_slot *s)
{
	if (!s->bis || !s->bis_locked) {
		uint32_t guard = OPEN_GUARD_BASE_US + s->interval_us / 1000;

		/* Short connection interval (6.2): when the interval drops to a few hundred µs, a fixed 300µs lead time would eat most of an
		 * event. The lead time takes at most ~1/3 of the interval, just enough to leave room for radio readiness (~56µs), with the rest
		 * left for the RX window itself. A locked ISO slot follows the separate schedule constraint below and isn't subject to this. */
		if (s->interval_us > 0u) {
			const uint32_t cap = s->interval_us / 3u;

			if (guard > cap && cap > ISO_RX_READY_US) {
				guard = cap;
			}
		}
		return guard;
	}

	uint32_t guard = ISO_LOCKED_GUARD_US;

	/* The lead-time upper bound is set by the schedule: window = lead time + full-packet on-air + tail margin, and the window must end before the next packet
	 * opens (two window-opens are exactly one schedule apart), i.e.
	 *     lead time + on-air + tail margin + rescheduling overhead ≤ schedule.
	 * Note this **no longer** deducts an extra radio-switch time — that cost happens inside the lead time. */
	if (s->bis_slot_spacing > 0u) {
		const uint32_t need = s->bis_pdu_air_us + ISO_TAIL_GUARD_US +
				      SCHED_LATE_LEAD_US;
		const uint32_t room = (s->bis_slot_spacing > need)
					      ? (s->bis_slot_spacing - need) : 0u;

		if (guard > room) {
			guard = room;
		}
	}
	return guard;
}

/* How many "events we actually listen to" must be missed in a row to deem loss.
 * Prefer the link's real supervision timeout; when subrating is in effect we only listen on subscribed events,
 * so each miss represents subrate_factor connection events' worth of time. */
static uint16_t supervision_limit(const struct conn_slot *s)
{
	if (s->bis) {
		return 50u;   /* ISO slot: no supervision timeout, give it plenty of chances to re-search the anchor */
	}
	if (s->timeout_10ms == 0 || s->interval_us == 0) {
		return SUPERVISION_MISS;
	}

	const uint32_t factor = (s->subrate_factor > 1u) ? s->subrate_factor : 1u;
	const uint32_t per_listen_us = s->interval_us * factor;
	const uint32_t limit = ((uint32_t)s->timeout_10ms * 10000u) / per_listen_us;

	if (limit < SUPERVISION_MIN) {
		return SUPERVISION_MIN;
	}
	if (limit > SUPERVISION_MAX) {
		return SUPERVISION_MAX;
	}
	return (uint16_t)limit;
}

/* Find the earliest upcoming event; returns the slot number, *open_us is filled with when its window should open. -1 if none. */
static int earliest_event(uint32_t *open_us)
{
	int best = -1;
	uint32_t best_open = 0;

	for (int i = 0; i < CONN_FOLLOW_MAX_SLOTS; i++) {
		if (!f.slots[i].active) {
			continue;
		}
		const struct conn_slot *s = &f.slots[i];
		const uint32_t open = s->next_mts - open_guard_us(s);

		/* Compare by "distance relative to the current time", naturally handling counter wraparound */
		if (best < 0 ||
		    (int32_t)(open - best_open) < 0) {
			best = i;
			best_open = open;
		}
	}

	if (best >= 0) {
		*open_us = best_open;
	}
	return best;
}

/* ------------------------------------------------ Extended-advertising AUX-chain following */

/* Parse the AuxPtr out of an extended-advertising PDU.
 * PDU: pdu[0]=header (low 4 bits type=0x07), pdu[1]=payload length,
 *      pdu[2]=ExtHdrLen (low 6 bits)+AdvMode (high 2 bits), pdu[3]=extended-header Flags,
 *      pdu[4..]=fields present bit by bit per Flags (AdvA/TargetA/CTEInfo/ADI/AuxPtr/…).
 * If AuxPtr is present, fill the out-params (channel / PHY / time offset µs) and return true. */
static bool parse_aux_ptr(const uint8_t *pdu, uint16_t pdu_len,
			  uint8_t *out_ch, uint8_t *out_phy, uint32_t *out_offset_us)
{
	if (pdu_len < 4) {
		return false;
	}
	const uint8_t payload_len = pdu[1];

	if (payload_len < 2 || (uint16_t)(2 + payload_len) > pdu_len) {
		return false;
	}
	const uint8_t ext_hdr_len = pdu[2] & 0x3F;   /* Excludes the pdu[2] byte itself */
	const uint8_t flags = pdu[3];

	if (ext_hdr_len < 1 ||
	    (uint16_t)(3 + ext_hdr_len) > (uint16_t)(2 + payload_len)) {
		return false;
	}
	if (!(flags & EXT_HDR_FLAG_AUXPTR)) {
		return false;
	}

	/* Locate AuxPtr: skip the bit-present fields before it */
	uint16_t idx = 4;

	if (flags & EXT_HDR_FLAG_ADVA) {
		idx += 6;
	}
	if (flags & EXT_HDR_FLAG_TARGETA) {
		idx += 6;
	}
	if (flags & EXT_HDR_FLAG_CTE) {
		idx += 1;
	}
	if (flags & EXT_HDR_FLAG_ADI) {
		idx += 2;
	}
	if ((uint16_t)(idx + 3) > pdu_len) {
		return false;
	}

	const uint8_t *ap = &pdu[idx];
	const uint8_t chan = ap[0] & 0x3F;
	const uint16_t units = (ap[0] & 0x80) ? 300u : 30u;
	const uint16_t w = (uint16_t)ap[1] | ((uint16_t)ap[2] << 8);
	const uint16_t aux_offset = w & 0x1FFF;
	const uint8_t aux_phy_raw = (uint8_t)((w >> 13) & 0x07);

	if (chan > 36) {
		return false;
	}
	switch (aux_phy_raw) {
	case 0:
		*out_phy = PHY_1M;
		break;
	case 1:
		*out_phy = PHY_2M;
		break;
	case 2:
		*out_phy = PHY_CODED_S8;
		break;
	default:
		return false;   /* RFU */
	}
	*out_ch = chan;
	*out_offset_us = (uint32_t)aux_offset * units;
	return true;
}

/* Called on receiving an extended-advertising packet: if it carries an AuxPtr, register the next-hop aux tracking (channel/PHY/instant). */
static void note_ext_adv(const struct radio_packet *pkt)
{
	uint8_t ch, phy;
	uint32_t off_us;

	/* While following a connection/periodic/BIS/CIS, don't chase opportunistic ext-adv — otherwise aux service would preempt the
	 * radio and miss a one-shot control packet within a connection (e.g. the LL_CIS_IND of CIS setup). Following takes priority. */
	if (count_active() > 0) {
		return;
	}
	if (f.aux_chain >= AUX_MAX_CHAIN) {
		return;   /* Chain too long, stop, to avoid an abnormal packet keeping the radio running away */
	}
	if (!parse_aux_ptr(pkt->pdu, pkt->pdu_len, &ch, &phy, &off_us)) {
		return;
	}
	if (off_us == 0 || off_us > AUX_MAX_OFFSET_US) {
		return;
	}
	f.aux_pending = true;
	f.aux_channel = ch;
	f.aux_phy = phy;
	f.aux_at_us = pkt->timestamp_us + off_us;
}

/* ------------------------------------------------ Periodic advertising sync (BLE 5.0) */

struct sync_params {
	uint32_t aa;
	uint32_t crc_init;
	uint16_t interval;         /* ×1.25ms */
	uint8_t chan_map[5];
	uint16_t event_counter;    /* The paEventCounter of the AUX_SYNC_IND that SyncInfo points to */
	uint32_t offset_us;        /* Offset from the carrier packet (AUX_ADV_IND) to the first AUX_SYNC_IND */
	uint8_t phy;               /* Periodic-train PHY = the PHY of the AUX packet carrying it */
	/* ---- PAwR (5.4): from the 0x32 field in the ACAD; num_subevents==0 means not PAwR ---- */
	uint32_t rsp_aa;           /* RspAA: the access address used by the response packet */
	uint8_t num_subevents;     /* Number of subevents in one periodic event (1..128) */
	uint32_t subevent_us;      /* Interval between adjacent subevents */
	uint32_t rsp_delay_us;     /* Subevent start → first response slot */
	uint32_t rsp_spacing_us;   /* Interval between adjacent response slots */
};

/* Parse the 18-byte core of a raw SyncInfo (shared by the advertising extended header and PAST). interval==0 is treated as invalid. */
static bool parse_sync_info_raw(const uint8_t *si, uint8_t phy, struct sync_params *sp)
{
	const uint16_t off = (uint16_t)si[0] | ((uint16_t)si[1] << 8);
	const uint16_t sync_off = off & 0x1FFF;
	const uint16_t units = (off & 0x2000) ? 300u : 30u;
	const bool adjust = (off & 0x4000) != 0;

	sp->offset_us = (uint32_t)sync_off * units + (adjust ? 2457600u : 0u);
	sp->interval = (uint16_t)si[2] | ((uint16_t)si[3] << 8);
	for (int i = 0; i < 5; i++) {
		sp->chan_map[i] = si[4 + i];
	}
	sp->chan_map[4] &= 0x1F;   /* The high 3 bits are SCA */
	sp->aa = (uint32_t)si[9] | ((uint32_t)si[10] << 8) |
		 ((uint32_t)si[11] << 16) | ((uint32_t)si[12] << 24);
	sp->crc_init = (uint32_t)si[13] | ((uint32_t)si[14] << 8) | ((uint32_t)si[15] << 16);
	sp->event_counter = (uint16_t)si[16] | ((uint16_t)si[17] << 8);
	sp->phy = phy;
	sp->num_subevents = 0;
	return sp->interval != 0;
}

/* Parse SyncInfo out of an extended-advertising PDU (18 bytes, extended-header Flags bit5). Its field order comes after AuxPtr. */
static bool parse_sync_info(const uint8_t *pdu, uint16_t pdu_len, uint8_t phy,
			    struct sync_params *sp)
{
	if (pdu_len < 4) {
		return false;
	}
	const uint8_t payload_len = pdu[1];

	if (payload_len < 2 || (uint16_t)(2 + payload_len) > pdu_len) {
		return false;
	}
	const uint8_t flags = pdu[3];

	if (!(flags & EXT_HDR_FLAG_SYNC)) {
		return false;
	}
	uint16_t idx = 4;

	if (flags & EXT_HDR_FLAG_ADVA) {
		idx += 6;
	}
	if (flags & EXT_HDR_FLAG_TARGETA) {
		idx += 6;
	}
	if (flags & EXT_HDR_FLAG_CTE) {
		idx += 1;
	}
	if (flags & EXT_HDR_FLAG_ADI) {
		idx += 2;
	}
	if (flags & EXT_HDR_FLAG_AUXPTR) {
		idx += 3;
	}
	if ((uint16_t)(idx + 18) > pdu_len) {
		return false;
	}

	const uint8_t *si = &pdu[idx];

	if (!parse_sync_info_raw(si, phy, sp)) {
		return false;
	}

	/* PAwR (5.4): the subevent/response-slot parameters are in the ACAD of the same extended header, AD type 0x32
	 * "Periodic Advertising Response Timing Information", 8 bytes:
	 *   RspAA(4) numSubevents(1) subeventInterval(×1.25ms) responseSlotDelay(×1.25ms)
	 *   responseSlotSpacing(×0.125ms)
	 * — that is, PAwR's layout **is available right on air**, no need to sniff PAST.
	 * (Cross-check: these four parameters are exactly the four reported by the HCI `LE Periodic Advertising Sync Established
	 *  [v2]` event, which also fires for a sync established via SyncInfo.) */
	sp->num_subevents = 0;
	const uint16_t ext_hdr_end = (uint16_t)(3 + (pdu[2] & 0x3F));
	uint16_t acad = (uint16_t)(idx + 18);

	while ((uint16_t)(acad + 1) < ext_hdr_end && ext_hdr_end <= pdu_len) {
		const uint8_t ad_len = pdu[acad];

		if (ad_len == 0 || (uint16_t)(acad + 1 + ad_len) > ext_hdr_end) {
			break;
		}
		if (pdu[acad + 1] == AD_TYPE_PAWR_TIMING && ad_len >= 9) {
			const uint8_t *d = &pdu[acad + 2];

			sp->rsp_aa = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
				     ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
			sp->num_subevents = d[4];
			sp->subevent_us = (uint32_t)d[5] * UNIT_1_25_MS_US;
			sp->rsp_delay_us = (uint32_t)d[6] * UNIT_1_25_MS_US;
			sp->rsp_spacing_us = (uint32_t)d[7] * UNIT_125_US;
			break;
		}
		acad += (uint16_t)(1 + ad_len);
	}
	return true;
}

/* Build a periodic-advertising tracking slot from SyncInfo (reusing the connection-slot machinery: CSA#2 + anchor resync).
 * Don't arm_next here — the call site is inside aux service, so let serve_aux_close's arm_next do the unified scheduling. */
static void start_periodic(const struct sync_params *sp, uint32_t carrier_ts)
{
	if (sp->interval == 0 || find_slot_by_aa(sp->aa) >= 0) {
		return;
	}
	const uint8_t chan_count = ble_csa_channel_count(sp->chan_map);

	if (chan_count < 2) {
		return;
	}
	const int idx = alloc_slot();

	if (idx < 0) {
		return;
	}
	struct conn_slot *s = &f.slots[idx];

	*s = (struct conn_slot){0};
	s->active = true;
	s->periodic = true;
	s->aa = sp->aa;
	s->crc_init = sp->crc_init;
	s->csa2 = true;                        /* Periodic advertising always uses CSA#2 */
	s->chan_id = ble_csa_channel_id(sp->aa);
	s->interval_us = (uint32_t)sp->interval * UNIT_1_25_MS_US;
	s->chan_count = chan_count;
	for (int i = 0; i < 5; i++) {
		s->chan_map[i] = sp->chan_map[i];
	}
	s->event_counter = sp->event_counter;
	s->phy = sp->phy;
	s->phy_slave = sp->phy;
	s->next_mts = carrier_ts + sp->offset_us;
	/* The first periodic packet uses a widened window to cover timing uncertainty */
	s->first_win_us = 2 * OPEN_GUARD_BASE_US + STEADY_EVENT_US + 1500u;

	if (sp->num_subevents > 1 && sp->subevent_us > 0) {
		/* PAwR (5.4): one periodic event contains multiple subevents. Its layout/channel derivation is exactly isomorphic to BIS's
		 * in-event multi-subevent (fixed anchor + equal spacing + CSA#2 subevent recurrence),
		 * so we directly reuse that ISO-subevent machinery, only with parameters coming from ACAD's 0x32 instead of BIGInfo. */
		s->pawr = true;
		s->bis_nse = sp->num_subevents;
		s->bis_sub_interval = sp->subevent_us;
		if (PAWR_FOLLOW_SUBEVENTS) {
			/* The subevent layout/time grid is isomorphic to BIS's in-event multi-subevent, so directly reuse
			 * that ISO-subevent scheduling; the only difference is the channel-derivation rule (see the notes above). */
			s->bis = true;
			s->bis_slot_spacing = sp->subevent_us;
			s->bis_event_us = s->next_mts;
			s->bis_anchor_us = s->next_mts;
			s->bis_anchor_cnt = s->event_counter;
			/* PAwR subevent PDU length is variable, so estimate the on-air duration at full length to set the window upper bound
			 * (the subevent interval is at least 7.5ms, which covers it) */
			s->bis_pdu_air_us = iso_pdu_air_us(sp->phy, 255u, false);
		}
		s->pawr_rsp_aa = sp->rsp_aa;
		s->pawr_rsp_delay_us = sp->rsp_delay_us;
		s->pawr_rsp_spacing_us = sp->rsp_spacing_us;
		LOG_INF("PAwR periodic train: AA=%08X interval=%ums subevents=%u×%uus "
			"response slots delay=%uus spacing=%uus RspAA=%08X "
			"chan_id=%04X map=%02X%02X%02X%02X%02X available=%u",
			s->aa, s->interval_us / 1000u, s->bis_nse,
			s->bis_sub_interval, s->pawr_rsp_delay_us,
			s->pawr_rsp_spacing_us, s->pawr_rsp_aa, s->chan_id,
			s->chan_map[0], s->chan_map[1], s->chan_map[2],
			s->chan_map[3], s->chan_map[4], s->chan_count);
	}

	f.stats.periodic_synced++;
	refresh_active_stats();
}

/* PAST (5.1): consume past_pending, compute the SyncInfo offset reference from the ACL anchor corresponding to syncConnEventCount,
 * and build a periodic-advertising slot. ⚠️ Not verified on real hardware (lacking a PAST central + a periodic source); the logic is the same
 * as building periodic tracking from an on-air SyncInfo (start_periodic), only the carrier anchor comes from the connection rather than an AUX packet. */
static void poll_past(void)
{
	if (!past_pending.valid) {
		return;
	}
	past_pending.valid = false;

	struct sync_params sp = {0};

	if (!parse_sync_info_raw(past_pending.syncinfo,
				 phy_from_mask(past_pending.phy_mask, PHY_1M), &sp)) {
		return;
	}
	if (find_slot_by_aa(sp.aa) >= 0) {
		return;   /* Already following this periodic train */
	}
	int32_t delta = (int32_t)(int16_t)(past_pending.sync_conn_event - past_pending.acl_event);

	if (delta < 0) {
		delta = 0;
	}
	const uint32_t carrier = past_pending.acl_anchor_us +
				 (uint32_t)delta * past_pending.acl_interval_us;

	LOG_INF("PAST: received LL_PERIODIC_SYNC_IND, building periodic tracking AA=%08X interval=%ums",
		sp.aa, (uint32_t)sp.interval * UNIT_1_25_MS_US / 1000u);
	start_periodic(&sp, carrier);
}

/* ------------------------------------------ BIS (LE Audio broadcast isochronous stream, 5.2) */

/* Get a value from a byte array by absolute bit offset (≤32 bits), corresponding to Zephyr util_get_bits. */
static uint32_t bi_bits(const uint8_t *d, uint16_t bit_offs, uint8_t num_bits)
{
	uint32_t v = 0;
	uint8_t shift = 0;

	while (num_bits) {
		const uint8_t bo = bit_offs & 7u;
		const uint8_t take = (num_bits < (8u - bo)) ? num_bits : (uint8_t)(8u - bo);

		v |= (uint32_t)((d[bit_offs >> 3] >> bo) & ((1u << take) - 1u)) << shift;
		shift += take;
		num_bits -= take;
		bit_offs += take;
	}
	return v;
}

/* Derive the access address of the bis-th BIS from the seed access address (bis=0 is the BIG Control).
 * Ported from Zephyr util_bis_aa_le32 (BT Core Spec 5.2 Vol6 PartB §2.1.2). */
static uint32_t bis_aa_derive(uint8_t bis, uint32_t saa)
{
	const uint8_t d = (uint8_t)(((35u * bis) + 42u) & 0x7fu);
	uint8_t dwh1 = (d & 1u) ? 0xFCu : 0x00u;

	dwh1 |= (uint8_t)((d & 0x02u) | ((d >> 6) & 0x01u));
	const uint8_t dwh0 = (uint8_t)(((d & 0x02u) << 6) | (d & 0x30u) | ((d & 0x0Cu) >> 1));

	return saa ^ (((uint32_t)dwh1 << 24) | ((uint32_t)dwh0 << 16));
}

/* Common parameters of one BIG, shared when building each BIS's slot */
struct big_params {
	uint32_t seed_aa;
	uint16_t base_crc;
	uint8_t chan_map[5];
	uint8_t chan_count;
	uint8_t phy;
	uint8_t nse;
	uint8_t bn;
	uint8_t num_bis;
	uint16_t event_counter;
	uint32_t interval_us;
	uint32_t sub_interval;    /* Interval between adjacent subevents of the same BIS */
	uint32_t bis_spacing;     /* Start-to-start interval between two adjacent BIS */
	uint32_t slot_spacing;    /* Interval between two adjacent on-air packets (see conn_slot.bis_slot_spacing) */
	uint32_t pdu_air_us;
	uint32_t big_anchor_us;   /* Packet-start time of subevent 0 of BIS #1 */
};

/* Build a tracking slot for the bis_index-th BIS (1-based) in a BIG. Returns true on success.
 *
 * Each BIS differs in only four places; the rest (channel map/PHY/interval/counter) is shared by the whole BIG:
 *   - access address: derived from the seed AA by the BIS index;
 *   - CRCInit: BaseCRCInit<<8 | bis_index;
 *   - CSA#2's chan_id: computed from that BIS's own AA — so each BIS has a distinct hop sequence;
 *   - time anchor: BIG anchor + (bis_index−1) × BIS_Spacing.
 *
 * The anchor formula is the same for both sequential and interleaved layouts:
 *   sequential:   offset(b,se) = sub_interval × ((b−1)×NSE + se), where BIS_Spacing = NSE×sub_interval
 *   interleaved:  offset(b,se) = BIS_Spacing × ((b−1) + se×NUM_BIS), where sub_interval = BIS_Spacing×NUM_BIS
 * Both expand to (b−1)×BIS_Spacing + se×sub_interval, so just use the raw BIGInfo fields directly,
 * without having to determine which layout it is (corresponding to those two branches in Zephyr lll_sync_iso.c). */
static bool start_bis_stream(const struct big_params *p, uint8_t bis_index)
{
	const uint32_t bis_aa = bis_aa_derive(bis_index, p->seed_aa);

	if (find_slot_by_aa(bis_aa) >= 0) {
		return false;   /* Already following this one */
	}
	const int slot = alloc_slot();

	if (slot < 0) {
		return false;
	}
	const uint32_t anchor = p->big_anchor_us +
				(uint32_t)(bis_index - 1u) * p->bis_spacing;

	struct conn_slot *s = &f.slots[slot];

	*s = (struct conn_slot){0};
	s->active = true;
	s->bis = true;
	s->aa = bis_aa;
	s->crc_init = ((uint32_t)p->base_crc << 8) | bis_index;
	s->csa2 = true;
	s->chan_id = ble_csa_channel_id(bis_aa);
	s->interval_us = p->interval_us;
	s->chan_count = p->chan_count;
	for (int i = 0; i < 5; i++) {
		s->chan_map[i] = p->chan_map[i];
	}
	s->event_counter = p->event_counter;
	s->phy = p->phy;
	s->phy_slave = p->phy;
	s->bis_nse = p->nse;
	s->bis_bn = p->bn;
	s->bis_num = p->num_bis;
	s->bis_index = bis_index;
	s->bis_sub_interval = p->sub_interval;
	s->bis_slot_spacing = p->slot_spacing;
	s->bis_pdu_air_us = p->pdu_air_us;
	s->next_mts = anchor;
	s->first_win_us = 4 * OPEN_GUARD_BASE_US + STEADY_EVENT_US + 2000u;
	/* Fixed anchor: afterwards each BIG event's counter is computed from here by real time, so skipped events don't lose step */
	s->bis_anchor_us = anchor;
	s->bis_anchor_cnt = p->event_counter;
	s->bis_event_us = anchor;

	f.stats.bis_synced++;
	LOG_INF("BIS#%u/%u slot built: AA=%08X CRCInit=%06X anchor+%uus slot=%uus NSE=%u",
		bis_index, p->num_bis, bis_aa, s->crc_init,
		(uint32_t)(bis_index - 1u) * p->bis_spacing, p->slot_spacing, p->nse);
	return true;
}

/* Number of free slots — determines how many BIS one BIG can follow in parallel */
static uint8_t free_slot_count(void)
{
	uint8_t n = 0;

	for (int i = 0; i < CONN_FOLLOW_MAX_SLOTS; i++) {
		if (!f.slots[i].active) {
			n++;
		}
	}
	return n;
}

/* Called when a periodic-advertising slot receives an AUX_SYNC_IND: look for BIGInfo in its ACAD, parse the parameters, and
 * build a tracking slot for each BIS in the BIG (if slots run short, follow the first few). carrier_ts = the RX time of that AUX_SYNC_IND. */
static void parse_and_start_bis(const struct radio_packet *pkt, uint32_t carrier_ts,
				uint8_t carrier_phy)
{
	const uint8_t *pdu = pkt->pdu;
	const uint16_t pdu_len = pkt->pdu_len;

	if (pdu_len < 4) {
		return;
	}
	const uint8_t payload_len = pdu[1];
	const uint8_t ext_hdr_len = pdu[2] & 0x3F;

	if (ext_hdr_len < 1 || (uint16_t)(3 + ext_hdr_len) > (uint16_t)(2 + payload_len)) {
		return;
	}
	const uint8_t flags = pdu[3];

	/* Skip the bit-present extended-header fields to locate the ACAD */
	uint16_t idx = 4;

	if (flags & EXT_HDR_FLAG_ADVA) {
		idx += 6;
	}
	if (flags & EXT_HDR_FLAG_TARGETA) {
		idx += 6;
	}
	if (flags & EXT_HDR_FLAG_CTE) {
		idx += 1;
	}
	if (flags & EXT_HDR_FLAG_ADI) {
		idx += 2;
	}
	if (flags & EXT_HDR_FLAG_AUXPTR) {
		idx += 3;
	}
	if (flags & EXT_HDR_FLAG_SYNC) {
		idx += 18;
	}
	if (flags & EXT_HDR_FLAG_TXPOWER) {
		idx += 1;
	}

	const uint16_t acad_end = (uint16_t)(3 + ext_hdr_len);   /* End of the extended header (exclusive) */

	if (idx > acad_end) {
		return;
	}

	/* Scan AD structures in the ACAD to find BIGInfo (type 0x2C) */
	const uint8_t *bi = NULL;
	uint8_t bi_len = 0;

	while ((uint16_t)(idx + 1) < acad_end) {
		const uint8_t ad_len = pdu[idx];

		if (ad_len == 0 || (uint16_t)(idx + 1 + ad_len) > acad_end) {
			break;
		}
		if (pdu[idx + 1] == AD_TYPE_BIG_INFO) {
			bi = &pdu[idx + 2];
			bi_len = (uint8_t)(ad_len - 1);
			break;
		}
		idx += (uint16_t)(1 + ad_len);
	}

	if (bi == NULL || bi_len < 33) {         /* BIGInfo plaintext is 33 bytes */
		return;
	}

	/* Parse the key BIGInfo fields (needed for subevent-0 following) */
	const uint32_t offs = bi_bits(bi, 0, 14);
	const uint8_t offs_units = (uint8_t)bi_bits(bi, 14, 1);
	const uint16_t iso_interval = (uint16_t)bi_bits(bi, 15, 12);
	const uint8_t num_bis = (uint8_t)bi_bits(bi, 27, 5);
	const uint8_t nse = (uint8_t)bi_bits(bi, 32, 5);
	const uint8_t bn = (uint8_t)bi_bits(bi, 37, 3);
	const uint32_t sub_interval = bi_bits(bi, 40, 20);
	const uint32_t bis_spacing = bi_bits(bi, 64, 20);
	const uint8_t max_pdu = bi[11];
	const uint32_t seed_aa = (uint32_t)bi[13] | ((uint32_t)bi[14] << 8) |
				 ((uint32_t)bi[15] << 16) | ((uint32_t)bi[16] << 24);
	const uint16_t base_crc = (uint16_t)bi[21] | ((uint16_t)bi[22] << 8);
	uint8_t chan_map[5];

	for (int i = 0; i < 5; i++) {
		chan_map[i] = bi[23 + i];
	}
	chan_map[4] &= 0x1F;                      /* The high 3 bits are PHY */
	const uint8_t phy_raw = (uint8_t)((bi[27] >> 5) & 0x07);
	const uint64_t payload_count = (uint64_t)bi[28] | ((uint64_t)bi[29] << 8) |
				       ((uint64_t)bi[30] << 16) | ((uint64_t)bi[31] << 24) |
				       ((uint64_t)(bi[32] & 0x7F) << 32);

	if (iso_interval == 0 || bn == 0 || num_bis == 0) {
		return;
	}
	const uint8_t chan_count = ble_csa_channel_count(chan_map);

	if (chan_count < 2) {
		return;
	}
	uint8_t bphy = PHY_1M;

	if (phy_raw == 1) {
		bphy = PHY_2M;
	} else if (phy_raw == 2) {
		bphy = PHY_CODED_S8;
	}

	/* BIG anchor = the "packet start" of AUX_SYNC_IND + BIG_Offset×unit.
	 *
	 * Following Zephyr ull_sync_iso.c:
	 *     sync_iso_offset_us  = ftr->radio_end_us;              // packet end
	 *     sync_iso_offset_us += BIG_Offset × unit;
	 *     sync_iso_offset_us -= PDU_AC_US(pdu->len, phy, ...);  // subtract the full-packet on-air duration
	 * "packet end − full-packet on-air duration" is the packet start, so BIG_Offset is referenced to the packet start, not the packet end.
	 *
	 * Our timestamp is stamped the instant AA finishes being received; subtract the preamble+AA duration to get the packet start.
	 * (The original implementation computed "packet end + offset", making the anchor systematically late by one whole packet duration: at 1M,
	 *  ~40µs preamble+AA + ~384µs packet body ≈ 424µs, exceeding the 310µs window lead time,
	 *  so the subevent always fell before the RX window opened → not a single BIS packet could be captured.) */
	const uint32_t units = offs_units ? OFFS_UNIT_300_US : OFFS_UNIT_30_US;

	/* How many can be followed in parallel this time: the min of the count in the BIG, the remaining slots, and the per-BIG limit */
	uint8_t want = (num_bis < BIS_MAX_STREAMS) ? num_bis : BIS_MAX_STREAMS;
	const uint8_t avail = free_slot_count();

	if (want > avail) {
		want = avail;
	}
	if (want == 0) {
		return;
	}

	struct big_params p = {
		.seed_aa = seed_aa,
		.base_crc = base_crc,
		.chan_count = chan_count,
		.phy = bphy,
		/* Number of subevents to walk within one BIG event. sub_interval == 0 (which the spec allows when NSE=1)
		 * degenerates to receiving only subevent 0. */
		.nse = (sub_interval > 0u && nse > 0u) ? nse : 1u,
		.bn = bn,
		.num_bis = num_bis,
		.event_counter = (uint16_t)((payload_count / bn) & 0xFFFFu),
		.interval_us = (uint32_t)iso_interval * UNIT_1_25_MS_US,
		.sub_interval = sub_interval,
		.bis_spacing = bis_spacing,
		/* An encrypted BIGInfo is 57 bytes (plaintext 33 + GIV 8 + GSKD 16); an encrypted stream adds 4 bytes of MIC per packet */
		.pdu_air_us = iso_pdu_air_us(bphy, max_pdu, bi_len >= 57),
		.big_anchor_us = carrier_ts - preamble_aa_us(carrier_phy) + offs * units,
	};

	for (int i = 0; i < 5; i++) {
		p.chan_map[i] = chan_map[i];
	}

	/* How far the "next packet" on air is from this one: the next subevent of the same BIS is sub_interval away;
	 * when following multiple in parallel, under interleaved layout other BIS are inserted in between (BIS_Spacing apart). Take the smaller to set the window.
	 * When neither applies (NSE=1 and only one being followed), leave 0, meaning no window upper bound. */
	if (p.nse > 1u && sub_interval > 0u) {
		p.slot_spacing = sub_interval;
	}
	if (want > 1u && bis_spacing > 0u &&
	    (p.slot_spacing == 0u || bis_spacing < p.slot_spacing)) {
		p.slot_spacing = bis_spacing;
	}

	uint8_t started = 0;

	for (uint8_t b = 1; b <= num_bis && started < want; b++) {
		if (start_bis_stream(&p, b)) {
			started++;
		}
	}

	if (started > 0) {
		refresh_active_stats();
	}
}

static void serve_aux_open(void)
{
	f.aux_pending = false;
	f.aux_chain++;

	radio_rx_stop();
	/* AUX packets still use the advertising AA + advertising CRCInit, just moved to a data channel with the PHY specified by AuxPtr */
	radio_set_phy((phy_t)f.aux_phy);
	radio_set_alt_phy((phy_t)f.aux_phy);
	radio_set_channel(f.aux_channel);
	radio_set_access_addr(BLE_ADV_ACCESS_ADDR);
	radio_set_crcinit(BLE_ADV_CRC_INIT);

	f.mode = MODE_SERVING_AUX;
	radio_rx_start();

	const bool coded = (f.aux_phy == PHY_CODED_S8 || f.aux_phy == PHY_CODED_S2);

	radio_sched_at(radio_now_us() + (coded ? AUX_WINDOW_CODED_US : AUX_WINDOW_US),
		       serve_aux_close);
}

static void serve_aux_close(void)
{
	radio_rx_stop();
	f.stats.ext_adv_chase++;
	/* If an AUX packet received within the window carries another AuxPtr, note_ext_adv has already set aux_pending,
	 * and arm_next will keep chasing the next hop (AUX_CHAIN_IND); otherwise this chain ends, and reset the chain depth. */
	if (!f.aux_pending) {
		f.aux_chain = 0;
	}
	arm_next();
}

static void serve_open(int idx)
{
	struct conn_slot *s = &f.slots[idx];
	/* Instrumentation: how much later than the scheduled window-open this dispatch was, and how long configuring the radio took.
	 * When a subevent's budget is only ~200µs, these two directly determine whether the next packet can be captured. */
	const uint32_t dbg_t0 = radio_now_us();
	const int32_t dbg_late = (int32_t)(dbg_t0 - (s->next_mts - open_guard_us(s)));

	/* Count only locked ISO slots — the "lateness" of a newly-built slot / wide-window re-search has no reference value */
	if (s->bis && s->bis_locked && dbg_late > (int32_t)f.stats.open_late_max_us) {
		f.stats.open_late_max_us = (uint32_t)dbg_late;
	}
	s->serves++;

	apply_pending_if_due(s);

	/* A PAwR response slot is on the same channel as the subevent it belongs to, just with the RspAA swapped in (see 4.4.2 and ACAD 0x32) */
	uint8_t ch;

	if (s->pawr && s->pawr_in_rsp) {
		ch = s->pawr_last_ch;
	} else {
		ch = s->bis ? bis_channel_for_subevent(s) : channel_for_event(s);
		s->pawr_last_ch = ch;
	}
	const bool coded = (s->phy == PHY_CODED_S8 || s->phy == PHY_CODED_S2 ||
			    s->phy_slave == PHY_CODED_S8 || s->phy_slave == PHY_CODED_S2);

	/* M4 relock: a normal connection missing several events = the anchor/PHY may be off due to an unparsed update. */
	const bool relocking = !s->bis && !s->pawr && !s->periodic &&
			       s->miss_count >= relock_after(s);
	/* An encrypted link can't see PHY_UPDATE_IND; when re-searching, alternate the master PHY between 1M/2M to hit the PHY the device switched to. */
	phy_t relock_phy = (phy_t)s->phy;

	if (relocking && s->encrypted) {
		relock_phy = (((s->miss_count - relock_after(s)) & 1u) != 0u) ? PHY_2M : PHY_1M;
	}

	radio_rx_stop();
	/* Set the master-direction PHY as primary; set the slave-direction PHY as alt. If they differ, the radio alternates packet by packet within the event
	 * (asymmetric link, e.g. C→P Coded, P→C 1M). If equal, it's symmetric and doesn't alternate. */
	radio_set_phy(relock_phy);
	radio_set_alt_phy((relocking && s->encrypted) ? relock_phy : (phy_t)s->phy_slave);
	radio_set_channel(ch);
	radio_set_access_addr((s->pawr && s->pawr_in_rsp) ? s->pawr_rsp_aa : s->aa);
	radio_set_crcinit(s->crc_init);

	f.serving_idx = idx;
	f.mode = MODE_SERVING;
	f.pkts_this_event = 0;
	f.got_master = false;
	f.got_data = false;

	radio_rx_start();

	const uint32_t dbg_cost = radio_now_us() - dbg_t0;

	if (dbg_cost > f.stats.open_cost_max_us) {
		f.stats.open_cost_max_us = dbg_cost;
	}
	f.stats.open_cost_sum_us += dbg_cost;
	f.stats.open_count++;

	const uint32_t guard = open_guard_us(s);
	/* Coded packets are much longer, so widen the event window accordingly */
	uint32_t window = 2 * guard + (coded ? CODED_EVENT_US : STEADY_EVENT_US);

	if (s->first_win_us != 0) {
		window = s->first_win_us;   /* The first event uses a widened window */
	}
	if (s->periodic && !s->bis && s->max_pdu_seen > 0 && s->first_win_us == 0) {
		/* Periodic-advertising slot: it has already received a packet (the anchor is accurate, measured anchor offset 0µs), so the window only needs to cover the
		 * longest PDU actually seen. The original 2.6ms "steady-state event window" was meant for the multiple round-trips within a connection slot's event;
		 * a periodic train has only one PDU per event (chain packets aren't chased within this window), so it wastes 3.3ms —
		 * blocking once every 60ms, exactly enough to knock out a whole train of BIS subevents. */
		window = guard + iso_pdu_air_us(s->phy,
						(uint16_t)(s->max_pdu_seen + 16u), false) +
			 ISO_TAIL_GUARD_US;
	} else if (s->pawr && s->pawr_in_rsp) {
		/* Response slot: the ACAD only gives the first response slot's delay and the slot spacing, **not the slot count**, so open from the first
		 * response slot all the way to just before the next subevent starts, sweeping up all responses of this subevent. */
		const uint32_t span = (s->bis_sub_interval > s->pawr_rsp_delay_us)
					      ? (s->bis_sub_interval - s->pawr_rsp_delay_us)
					      : s->bis_sub_interval;

		window = (span > ISO_RX_READY_US) ? (span - ISO_RX_READY_US) : span;
	} else if (s->bis) {
		if (s->bis_locked) {
			/* Already anchored: the window only needs "lead time + full-packet on-air + a little tail margin"; every extra µs is
			 * stolen from the next packet's switch budget. */
			window = guard + s->bis_pdu_air_us + ISO_TAIL_GUARD_US;

			/* Failsafe: the window must close before the next packet's window-open time, otherwise serve_close→
			 * arm_next is forever late and not a single later subevent / sibling BIS can be captured.
			 * When there are no more packets later (slot_spacing==0), don't set this upper bound. */
			if (s->bis_slot_spacing > SCHED_LATE_LEAD_US) {
				const uint32_t cap = s->bis_slot_spacing - SCHED_LATE_LEAD_US;

				if (window > cap) {
					window = cap;
				}
			}
		} else {
			/* Not yet anchored on a real ISO packet: the anchor is only derived from BIGInfo's BIG_Offset (or the CIS
			 * offset), and the main error is the 30/300µs quantization, so first use a window that just covers it to search
			 * for subevent 0; if the search keeps failing, widen gradually, at worst back to nearly the whole ISO interval.
			 *
			 * We can't use a wide window of the whole interval right away: that would monopolize the radio for nearly one event, and
			 * sibling BIS in the same BIG would all be starved — measured, when two BIS run in parallel, the later-built one
			 * would go 50 events without capturing a single packet and be deemed lost outright. */
			const uint8_t widen = (s->miss_count / BIS_SEARCH_WIDEN_EVERY < 5u)
						      ? (uint8_t)(s->miss_count /
								  BIS_SEARCH_WIDEN_EVERY)
						      : 5u;
			uint32_t span = (2u * OFFS_UNIT_300_US) << widen;
			const uint32_t span_max = (s->interval_us > 2000u)
							  ? (s->interval_us - 1000u)
							  : s->interval_us;

			window = 2 * guard + s->bis_pdu_air_us + span;
			if (window > span_max) {
				window = span_max;
			}
		}
	}

	/* M4 relock: a normal connection widens the window when re-searching (exponential growth); the "≤ one interval" cap below bounds the upper limit,
	 * covering the small anchor offset from an unparsed update, clock drift, and noise losses. */
	if (relocking) {
		const uint32_t w = (s->miss_count - relock_after(s) < RELOCK_WIDEN_MAX)
					   ? (uint32_t)(s->miss_count - relock_after(s))
					   : RELOCK_WIDEN_MAX;
		const uint32_t relock_win = 2u * guard + (RELOCK_BASE_SPAN_US << w);

		if (relock_win > window) {
			window = relock_win;
		}
	}

	/* Short connection interval (6.2): a normal connection slot's window (including the first window) must never overrun the next event's anchor,
	 * otherwise serve_close is late and the later events step off track one after another. The steady-state 2.6ms window is meant for the multiple round-trips within
	 * an event of a 7.5ms+ connection; when the interval drops to 375µs~a few ms it must be narrowed to within the interval.
	 * ISO/PAwR/periodic slots each have their own schedule constraint (handled above), so this only narrows normal connection slots. */
	if (!s->bis && !s->pawr && !s->periodic && s->interval_us > 0u) {
		const uint32_t cap = (s->interval_us > SCHED_LATE_LEAD_US)
					     ? (s->interval_us - SCHED_LATE_LEAD_US)
					     : s->interval_us;

		if (window > cap) {
			window = cap;
		}
	}

	/* When the window is due, directly schedule serve_close (radio_sched_at accepts any callback; there is only one
	 * pending schedule at a time: serve_open→serve_close, serve_close→arm_next→sched_dispatch). */
	radio_sched_at(radio_now_us() + window, serve_close);
}

static void serve_close(void)
{
	radio_rx_stop();

	struct conn_slot *s = &f.slots[f.serving_idx];

	/* PAwR: once a subevent is received, immediately go receive its response slot (same channel, RspAA swapped), then advance to
	 * the next subevent. The response slot doesn't participate in liveness/loss accounting — whether a device responds is not up to us,
	 * and using it to judge loss would kill a perfectly good periodic train. */
	const bool was_rsp = s->pawr && s->pawr_in_rsp;

	if (was_rsp) {
		s->pawr_in_rsp = false;   /* Response slot received, back to normal subevent advancement */
		if (f.got_data) {
			f.stats.pawr_responses++;
		}
	} else if (s->pawr && s->bis_locked && s->pawr_rsp_delay_us > 0 &&
		   s->pawr_rsp_delay_us < s->bis_sub_interval) {
		s->pawr_in_rsp = true;
		s->next_mts = s->bis_event_us +
			      (uint32_t)s->bis_se_idx * s->bis_sub_interval +
			      s->pawr_rsp_delay_us;
		if (f.got_data) {
			s->bis_ev_hits++;   /* The subevent itself hit, record it first */
		}
		arm_next();
		return;
	}

	/* Timing anchor: any received packet (even a CRC failure) is a valid anchor — used to align the next event.
	 * Liveness: an ISO slot requires "CRC-OK data received", otherwise (unable to get a valid ISO packet) deem loss quickly,
	 * to not needlessly preempt the radio time of connection/periodic following; connection/periodic slots still judge liveness by "any packet received". */
	const bool alive = s->bis ? f.got_data : f.got_master;
	/* Is this service the last subevent within an event? An ISO slot's liveness/loss must be accounted per whole
	 * event — of the NSE subevents in an event, hitting just one counts as followed; recording a miss per subevent would let
	 * BIS_RELOCK_MISS misfire even in a normal stream. */
	const bool iso_event_end = !s->bis || !s->bis_locked ||
				   (uint8_t)(s->bis_se_idx + 1u) >= s->bis_nse;

	if (s->bis && alive && !was_rsp) {
		s->bis_ev_hits++;
	}

	/* Account first (liveness/loss/whether the lock dropped), then schedule the next receive accordingly — a lock drop must be
	 * immediately reflected in the "return to subevent 0 and re-search with a wide window" scheduling. */
	if (!s->bis || iso_event_end) {
		const bool event_alive = s->bis ? (s->bis_ev_hits > 0) : alive;

		if (event_alive) {
			if (!s->bis && s->miss_count >= relock_after(s)) {
				f.stats.relock_recovered++;   /* Re-acquired after widening the window */
			}
			s->miss_count = 0;
			s->events++;
			f.stats.conn_events++;
		} else {
			s->miss_count++;
			f.stats.events_missed++;
			/* ISO missing several events in a row = the anchor may already have drifted, fall back to a wide window to re-find it */
			if (s->bis && s->miss_count >= BIS_RELOCK_MISS) {
				s->bis_locked = false;
			}
			if (s->miss_count >= supervision_limit(s)) {
				s->active = false;
				f.stats.lost++;
				if (!s->bis && !s->periodic) {
					g_acl_lost_timeouts++;
				}
				refresh_active_stats();
				LOG_INF("slot AA=%08X deemed lost (missed %u in a row, BIS#%u, was locked=%d, "
					"captured %u packets)", s->aa, s->miss_count,
					s->bis_index, (int)s->bis_locked,
					s->data_packets);
			}
		}
	}

	/* ---- Schedule the next receive ---- */
	if (s->bis) {
		s->first_win_us = 0;

		if (!iso_event_end) {
			/* There are still subevents to receive within this event: schedule the next one by sub_interval,
			 * event_counter unchanged (the whole event shares one counter). */
			s->bis_se_idx++;
			s->next_mts = s->bis_event_us +
				      (uint32_t)s->bis_se_idx * s->bis_sub_interval;
		} else {
			/* This event is done (or not yet locked, only searching subevent 0): jump to the next event.
			 * Both the time and event_counter are computed from "fixed anchor + real elapsed time", so
			 * events skipped (radio collision) don't make the counter lose step. */
			s->bis_se_idx = 0;
			s->bis_ev_hits_last = s->bis_ev_hits;   /* For diagnostics: how many were captured this event */
			s->bis_ev_hits = 0;
			const int32_t d = (int32_t)(radio_now_us() - s->bis_anchor_us);
			const uint32_t k = (d > 0 ? ((uint32_t)d / s->interval_us) : 0u) + 1u;

			s->bis_event_us = s->bis_anchor_us + k * s->interval_us;
			s->next_mts = s->bis_event_us;
			s->event_counter = (uint16_t)(s->bis_anchor_cnt + k);
		}
	} else if (f.got_master) {
		s->next_mts = f.master_ts + s->interval_us;
		s->first_win_us = 0;   /* Locked, use a narrow window afterwards */
	} else {
		s->next_mts += s->interval_us;
	}

	if (s->active) {
		if (!s->bis) {
			s->event_counter++;   /* The ISO counter was already computed by real time above */
			subrate_skip(s);
		}
		apply_pending_if_due(s);
	}

	arm_next();
}

/* Schedule the next action: either go serve the earliest event, or (during a gap / with no connections) scan */
static void arm_next(void)
{
	uint32_t open_us = 0;   /* When idx < 0, earliest_event doesn't fill it, and it isn't used later either */
	const int idx = earliest_event(&open_us);
	const uint32_t now = radio_now_us();

	/* Extended-advertising AUX tracking: lower priority than connection events, higher than pure scanning.
	 * Only chase when there is no earlier connection event; drop anything long overdue; if chasing too hard, get throttled. */
	if (f.aux_pending && single_target_locked()) {
		f.aux_pending = false;   /* Single-target locked: don't chase other extended-advertising chains */
		f.aux_chain = 0;
	}
	if (f.aux_pending) {
		const uint32_t aux_open = f.aux_at_us - AUX_OPEN_GUARD_US;

		if ((int32_t)(f.aux_at_us - now) < -(int32_t)AUX_WINDOW_US) {
			f.aux_pending = false;   /* Missed it, drop this hop */
			f.aux_chain = 0;
		} else if (idx < 0 || (int32_t)(aux_open - open_us) < 0) {
			if (!aux_budget_take(now)) {
				f.aux_pending = false;   /* Budget exhausted, don't chase this chain */
				f.aux_chain = 0;
				f.stats.ext_adv_throttled++;
			} else {
				const int32_t gap = (int32_t)(aux_open - now);

				f.next_action = SCHED_SERVE_AUX;
				radio_sched_at(gap < 0 ? now + SCHED_LATE_LEAD_US
						       : aux_open,
					       sched_dispatch);
				return;
			}
		}
	}

	if (idx < 0) {
		/* No active connections: pure scanning. Ensure the radio is scanning, and schedule one channel rotation. */
		if (f.mode != MODE_SCANNING) {
			enter_scan_radio();
		}
		if (f.scan_hopping) {
			f.next_action = SCHED_SCAN_ROTATE;
			radio_sched_at(now + SCAN_DWELL_US, sched_dispatch);
		} else {
			f.next_action = SCHED_NONE;
		}
		return;
	}

	/* If already late (the radio was previously busy with another connection and missed this event), record a collision */
	if ((int32_t)(open_us - now) < 0) {
		f.stats.collisions++;
	}

	const int32_t gap = (int32_t)(open_us - now);

	if (gap > (int32_t)SCAN_SLICE_MIN_US) {
		if (single_target_locked()) {
			/* Single-target mode: a connection is locked, don't return to the advertising channels during a gap — don't look at other devices, don't take new connections.
			 * Just stop receiving; after the slot is released, the idx<0 branch will re-enter enter_scan_radio to take the target's reconnect. */
			if (f.mode != MODE_IDLE) {
				radio_rx_stop();
				f.mode = MODE_IDLE;
			}
		} else if (f.mode != MODE_SCANNING) {
			/* Still a while until the next event, use the gap to scan. Only rotate to another advertising channel when round-robin is allowed;
			 * the tri-device stubborn-guard mode must stay on the strap-assigned channel (otherwise after following a connection once, the guard channel would be rotated away,
			 * and the 2026-09-19 on-board test had the three devices guarding 38/37/38, with no one guarding 39). */
			f.scan_channel = scan_channel_after_gap(f.scan_channel, f.scan_hopping);
			enter_scan_radio();
		}
	}

	f.next_action = SCHED_SERVE;
	f.next_slot = idx;
	/* If open has passed, trigger as soon as possible (clamp to a bit after the current time) */
	radio_sched_at((gap < 0) ? (now + SCHED_LATE_LEAD_US) : open_us, sched_dispatch);
}

static void sched_dispatch(void)
{
	switch (f.next_action) {
	case SCHED_SERVE:
		serve_open(f.next_slot);
		break;
	case SCHED_SERVE_AUX:
		serve_aux_open();
		break;
	case SCHED_SCAN_ROTATE:
		if (f.scan_hopping) {
			f.scan_channel = scan_next_adv_channel(f.scan_channel);
			if (f.mode == MODE_SCANNING) {
				radio_rx_stop();
				radio_set_channel(f.scan_channel);
				radio_rx_start();
			}
		}
		arm_next();
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------ RX entry point */

void conn_follower_poll_host_requests(void)
{
	poll_inject();   /* Tri-device co-follow: consume any FOLLOW relayed from the host (safe to build the slot in the radio interrupt / with interrupts off) */
	poll_hint();     /* Key hint: an encrypted control PDU the host decrypted, registered on the matching slot */
}

void conn_follower_on_packet(const struct radio_packet *pkt)
{
	conn_follower_poll_host_requests();
	poll_past();     /* PAST: consume an LL_PERIODIC_SYNC_IND received within a connection, build periodic tracking */

	if (f.mode == MODE_SERVING) {
		struct conn_slot *s = &f.slots[f.serving_idx];

		f.pkts_this_event++;
		if (pkt->pdu_len > s->max_pdu_seen) {
			s->max_pdu_seen = pkt->pdu_len;
		}
		if (f.pkts_this_event == 1) {
			f.got_master = true;
			f.master_ts = pkt->timestamp_us;
			/* Diagnostics: the deviation of this event's first packet relative to the predicted anchor. next_mts is only updated after
			 * serve_close, so right now next_mts is still this event's predicted anchor.
			 * An ISO slot's next_mts records the "packet start", while the RX timestamp is the instant AA finished being received;
			 * subtract preamble+AA for the same basis — otherwise Coded would be off by 376µs for nothing. */
			const uint32_t pkt_start = s->bis
				? (pkt->timestamp_us - preamble_aa_us(s->phy))
				: pkt->timestamp_us;

			s->anchor_delta_us = (int32_t)(pkt_start - s->next_mts);
		}
		if (!pkt->crc_ok) {
			s->crc_errors++;
		}
		if (pkt->crc_ok) {
			f.got_data = true;
			s->data_packets++;
			f.stats.data_packets++;
			if (!s->bis && !s->periodic) {
				g_acl_packets++;
			}
#if PAWR_CHAN_PROBE
			if (s->pawr && s->bis_se_idx > 0) {
				LOG_INF("PAwR sample %u %u %u", s->event_counter,
					s->bis_se_idx, s->probe_ch);
			}
#endif
			if (s->pawr && s->pawr_in_rsp) {
				/* A response packet is offset by responseSlotDelay relative to the subevent start; re-anchoring on it
				 * would skew the whole time grid — only receive it, don't touch the anchor. */
			} else if (s->bis) {
				/* Captured a CRC-correct ISO packet = the current event_counter is confirmed correct,
				 * so refresh the anchor to this confirmed position, correcting clock drift.
				 * The anchor is defined as "the **packet start** of subevent 0 of this event", so subtract
				 * preamble+AA (the timestamp-basis difference) and the current subevent's offset. */
				s->bis_anchor_us = pkt->timestamp_us -
						   preamble_aa_us(s->phy) -
						   (uint32_t)s->bis_se_idx * s->bis_sub_interval;
				s->bis_anchor_cnt = s->event_counter;
				s->bis_event_us = s->bis_anchor_us;
				s->bis_locked = true;
			} else if (pkt->pdu[1] > 0) {
				/* subrating: a packet with payload appeared on a subscribed event, so for the next continuationNumber
				 * events both sides keep transmitting/receiving — these events can't be skipped. */
				s->cont_left = s->subrate_cont;
			}
			if (s->periodic) {
				/* AUX_SYNC_IND: find BIGInfo in the ACAD → follow BIS (LE Audio) */
				parse_and_start_bis(pkt, pkt->timestamp_us, s->phy);
			} else if (!s->bis) {
				/* Only connection slots parse LL control packets; BIS slots receive ISO data, so skip */
				handle_data_pdu(s, pkt);
			}
		}

		/* Once an ISO subevent's packets are fully received, close the window immediately rather than waiting for the window timeout — the time saved
		 * turns directly into budget for switching the radio to the next subevent / sibling BIS. When the subevent interval is only
		 * ~200µs, these tens of µs are the difference between "capturing 1/2 or 2/2 per event".
		 * A BIS subevent has one packet; a CIS has two, C→P + P→C. */
		if (s->bis && s->bis_locked && !(s->pawr && s->pawr_in_rsp)) {
			/* A BIS / PAwR subevent has one packet; a CIS has two, C→P + P→C.
			 * A PAwR response window may contain multiple response slots, so don't close the window on receiving just one. */
			const uint8_t want = (s->bis_index > 0u || s->pawr) ? 1u : 2u;

			if (f.pkts_this_event >= want) {
				radio_sched_cancel();
				serve_close();
			}
		}
		return;
	}

	if (f.mode == MODE_SERVING_AUX) {
		/* The chased AUX packet has already been auto-forwarded to the host by main; here we check whether it:
		 *  (0) is an AUX_CONNECT_REQ (connection via extended advertising, 5.0; same type/same format as CONNECT_IND,
		 *      just occurring on a secondary channel) → start following, with the initial PHY = the secondary-channel PHY;
		 *  (1) chains one more hop (an AUX_CHAIN_IND with a new AuxPtr inside);
		 *  (2) carries SyncInfo → sync a periodic advertising train (BLE 5.0). */
		if (pkt->crc_ok && (pkt->pdu[0] & 0x0F) == ADV_PDU_CONNECT_IND &&
		    pkt->pdu_len >= 2 + 34) {
			radio_sched_cancel();
			start_following_aux(pkt);   /* The arm_next inside build_slot will take over scheduling */
			if (conn_follower_active_connections() == 0) {
				arm_next();             /* No slot was built (full/rejected): wind down as usual and return to scanning */
			}
			return;
		}
		if (pkt->crc_ok && (pkt->pdu[0] & 0x0F) == ADV_PDU_EXT_IND) {
			struct sync_params sp;

			note_ext_adv(pkt);
			if (parse_sync_info(pkt->pdu, pkt->pdu_len, f.aux_phy, &sp)) {
				start_periodic(&sp, pkt->timestamp_us);
			}
		}
		return;
	}

	/* Scanning state: find CONNECT_IND to build a new slot */
	if (pkt->crc_ok && (pkt->pdu[0] & 0x0F) == ADV_PDU_CONNECT_IND &&
	    pkt->pdu_len >= 2 + 34) {
		start_following(pkt);
		return;
	}

	/* Scanning state: a primary-channel ADV_EXT_IND with an AuxPtr → chase to the secondary channel to capture AUX_ADV_IND (BLE 5.0) */
	if (pkt->crc_ok && (pkt->pdu[0] & 0x0F) == ADV_PDU_EXT_IND) {
		note_ext_adv(pkt);
		if (f.aux_pending) {
			arm_next();   /* Re-plan: may switch to chasing aux */
		}
		return;
	}

	/* Decision-based advertising filtering (6.0): the newly added ADV_DECISION_IND (type 0x09) on the primary channel. The scanner could use
	 * the decision data inside to decide whether to chase AUX on the secondary channel; the sniffer is the passive side, taking everything and forwarding it to
	 * the host (parsed by Wireshark), and here we only count it to observe that it indeed appears on air. */
	if (pkt->crc_ok && (pkt->pdu[0] & 0x0F) == ADV_PDU_DECISION_IND) {
		f.stats.adv_decision++;
	}
}
