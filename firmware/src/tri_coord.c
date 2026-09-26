/*
 * Tri-board coordination layer implementation -- see tri_coord.h. Platform independent; only talks to
 * sync_line / peer_link / peer_msg / conn_follower.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "tri_coord.h"
#include "sync_line.h"
#include "peer_link.h"
#include "peer_msg.h"
#include "peer_txq.h"
#include "peer_anchor.h"
#include "conn_follower.h"

LOG_MODULE_REGISTER(tri_coord, LOG_LEVEL_INF);

#define ADV_PDU_CONNECT_IND 0x05
#define ADV_HDR_CHSEL2_BIT  0x20        /* ChSel bit of the advertising PDU header: 1 = the initiator supports CSA#2 */
#define SYNC_HEARTBEAT_US   1000000U    /* board 0 periodic time-base heartbeat: once a second (3.3) */
#define LINK_TEST_PERIOD_MS 10000       /* inter-board link self-test: every 10 s each board sends one HIT down both links */
#define LINK_TEST_FIRST_MS  2000
#define LINK_TEST_AA        0x5E1F7E57u /* marker AA of the self-test HIT ("self-test") */

static uint8_t g_board_id;
static struct tri_coord_stats g_stats;
static uint32_t g_last_heartbeat_us;

/* Inter-board sends go "ISR enqueues -> system work queue thread does the SPI": the master port transaction is
 * synchronous (bit-banged, ~0.7 ms), which is illegal in the radio RX interrupt and would also delay the
 * follower's slot setup/scheduling. The ISR only encodes + enqueues + k_work_submit (ISR safe), microseconds. */
static struct peer_txq g_txq;
static struct k_work g_peer_tx_work;
static struct k_work_delayable g_link_test_work;

/* Send one encoded message down both links: upstream via REQ (asynchronous, first), downstream via the master
 * port write (synchronous). */
static bool broadcast_msg(uint8_t *buf, size_t n)
{
	const int up = peer_link_send_upstream(buf, n);
	const int down = peer_link_send(buf, n);

	return up == 0 || down == 0;
}

static void peer_tx_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	uint8_t buf[PEER_MSG_MAX];
	uint32_t ref_tick;
	size_t n;

	while ((n = peer_txq_pop(&g_txq, buf, sizeof(buf), &ref_tick)) > 0) {
		if (buf[0] == PEER_MSG_HANDOFF) {
			/* Right before sending, fill in "how long since the hit SYNC edge"; the receiver uses it to
			 * recognise the edge (peer_anchor.h). */
			(void)peer_msg_handoff_set_age(buf, n, radio_now_us() - ref_tick);
		}
		if (broadcast_msg(buf, n) && buf[0] == PEER_MSG_HANDOFF) {
			g_stats.handoffs_sent++;
		}
	}
}

/* ISR context: enqueue and wake the sending thread. Dropped when the queue is full (counted, visible in the stats line). */
static void peer_enqueue(const uint8_t *msg, size_t len, uint32_t ref_tick)
{
	if (peer_txq_push(&g_txq, msg, len, ref_tick)) {
		k_work_submit(&g_peer_tx_work);
	}
}

/* Link self-test: from thread context send one HIT down both links; every board should see peer hits +2 per period. */
static void link_test_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	uint8_t buf[PEER_MSG_MAX];
	const struct peer_hit hit = {
		.board_id = g_board_id,
		.aa = LINK_TEST_AA,
		.anchor_sync_tick = radio_now_us(),
	};
	const size_t n = peer_msg_encode_hit(&hit, buf, sizeof(buf));

	if (n > 0) {
		(void)broadcast_msg(buf, n);
	}
	k_work_schedule(&g_link_test_work, K_MSEC(LINK_TEST_PERIOD_MS));
}

static bool is_adv_channel(uint8_t ch)
{
	return ch >= BLE_CHANNEL_ADV_37;
}

/* Parse the AA out of a CONNECT_IND (same offset as conn_follower's start_following) */
static uint32_t parse_connect_aa(const struct radio_packet *pkt)
{
	const uint8_t *ll = &pkt->pdu[2 + 12];   /* header/len(2) + InitA(6) + AdvA(6) */

	return (uint32_t)ll[0] | ((uint32_t)ll[1] << 8) |
	       ((uint32_t)ll[2] << 16) | ((uint32_t)ll[3] << 24);
}

/* Build a handoff: parameters from the CONNECT_IND; anchor = event0 anchor relative to this hit's SYNC edge (emit_tick). */
static void build_handoff(const struct radio_packet *pkt, uint32_t emit_tick, struct peer_handoff *ho)
{
	const uint8_t *ll = &pkt->pdu[2 + 12];

	*ho = (struct peer_handoff){0};
	ho->aa = parse_connect_aa(pkt);
	ho->crc_init = (uint32_t)ll[4] | ((uint32_t)ll[5] << 8) | ((uint32_t)ll[6] << 16);
	ho->win_size = ll[7];
	ho->interval = (uint16_t)ll[10] | ((uint16_t)ll[11] << 8);
	ho->latency = (uint16_t)ll[12] | ((uint16_t)ll[13] << 8);
	ho->timeout = (uint16_t)ll[14] | ((uint16_t)ll[15] << 8);
	ho->ch_map_lo = (uint32_t)ll[16] | ((uint32_t)ll[17] << 8) |
			((uint32_t)ll[18] << 16) | ((uint32_t)ll[19] << 24);
	ho->ch_map_hi = ll[20] & 0x1F;
	ho->hop = ll[21] & 0x1F;
	ho->csa2 = (pkt->pdu[0] & ADV_HDR_CHSEL2_BIT) ? 1u : 0u;
	ho->anchor_offset_us = (int32_t)(conn_follower_connect_ind_anchor0(pkt) - emit_tick);
	ho->send_age_us = 0;   /* filled by the sending thread right before the transaction */
}

/* Handoff received: recognise "the edge the discovering board pulled when it hit" among the recent captures,
 * convert the anchor and hand the connection to the follower. */
static void take_handoff(const struct peer_handoff *ho)
{
	uint32_t caps[SYNC_LINE_RECENT_MAX];
	const uint32_t now = radio_now_us();
	const uint32_t expect = now - ho->send_age_us;
	const size_t n = sync_line_recent_captures(caps, SYNC_LINE_RECENT_MAX);
	uint32_t edge;

	if (!peer_anchor_match(caps, n, expect, PEER_ANCHOR_MATCH_TOL_US, &edge)) {
		g_stats.handoffs_unmatched++;
		LOG_WRN("handoff AA=%08X: no SYNC capture near %u (age %u, newest capture %u, n=%u)",
			ho->aa, expect, ho->send_age_us, n > 0 ? caps[0] : 0u, (unsigned int)n);
		return;
	}

	struct conn_follow_inject inj = {
		.aa = ho->aa,
		.crc_init = ho->crc_init,
		.chan_map = {
			(uint8_t)(ho->ch_map_lo & 0xFF), (uint8_t)((ho->ch_map_lo >> 8) & 0xFF),
			(uint8_t)((ho->ch_map_lo >> 16) & 0xFF), (uint8_t)((ho->ch_map_lo >> 24) & 0xFF),
			ho->ch_map_hi,
		},
		.hop = ho->hop,
		.csa2 = ho->csa2,
		.interval = ho->interval,
		.latency = ho->latency,
		.timeout = ho->timeout,
		.win_size = ho->win_size,
		.anchor0_us = peer_anchor_local(edge, ho->anchor_offset_us),
	};

	conn_follower_request_inject(&inj);
	g_stats.handoffs_injected++;
	LOG_INF("handoff AA=%08X via inter-board link: edge %u (expect %u, age %u) -> anchor0 %u, now %u",
		ho->aa, edge, expect, ho->send_age_us, inj.anchor0_us, now);
}

/* A peer message arrived (SPIS interrupt / REQ read thread). */
static void on_peer_recv(const uint8_t *msg, size_t len)
{
	struct peer_hit hit;
	struct peer_handoff ho;
	const int type = peer_msg_decode(msg, len, &hit, &ho);

	if (type == PEER_MSG_HIT) {
		g_stats.peer_hits++;
	} else if (type == PEER_MSG_HANDOFF) {
		g_stats.handoffs_recv++;
		take_handoff(&ho);
	} else {
		g_stats.rx_bad++;
	}
}

void tri_coord_init(uint8_t board_id)
{
	g_board_id = board_id;

	(void)sync_line_init(NULL);          /* capture ticks are kept inside sync_line; periodic/frame header read them */
	(void)peer_link_init(on_peer_recv);
	peer_txq_init(&g_txq);
	k_work_init(&g_peer_tx_work, peer_tx_work_fn);
	k_work_init_delayable(&g_link_test_work, link_test_work_fn);
	k_work_schedule(&g_link_test_work, K_MSEC(LINK_TEST_FIRST_MS));
	conn_follower_set_claim_gate(tri_coord_should_follow);

	LOG_INF("tri-board coordination ready: board_id=%u (handoff over inter-board link to both peers)", board_id);
}

void tri_coord_on_packet(const struct radio_packet *pkt)
{
	/* Only CRC-correct CONNECT_INDs on this board's advertising channel (anchor hit). */
	if (!pkt->crc_ok || !is_adv_channel(pkt->channel)) {
		return;
	}
	if (pkt->pdu_len < 2 + 22 || (pkt->pdu[0] & 0x0F) != ADV_PDU_CONNECT_IND) {
		return;
	}

	g_stats.own_hits++;

	/* P2: pull a SYNC edge to give all three boards a common time-base anchor; emit_tick is when this board saw it. */
	const uint32_t emit_tick = sync_line_emit();

	g_stats.sync_emits++;

	/* Plan A: hand the connection parameters to the other two boards (anchor relative to this edge). The actual
	 * SPI transactions happen in the sending thread. */
	uint8_t buf[PEER_MSG_MAX];
	struct peer_handoff ho;

	build_handoff(pkt, emit_tick, &ho);

	const size_t n = peer_msg_encode_handoff(&ho, buf, sizeof(buf));

	if (n > 0) {
		peer_enqueue(buf, n, emit_tick);
	}
}

void tri_coord_periodic(void)
{
	/* Only board 0 sends the periodic time-base heartbeat so three boards do not stack edges on the open-drain line.
	 * On a hit any board still pulls its own edge (see on_packet) -- "whoever catches it pulls" is unaffected. */
	if (g_board_id != 0) {
		return;
	}

	const uint32_t now = radio_now_us();

	if ((uint32_t)(now - g_last_heartbeat_us) < SYNC_HEARTBEAT_US) {
		return;
	}
	g_last_heartbeat_us = now;

	(void)sync_line_emit();
	g_stats.sync_emits++;
}

uint32_t tri_coord_sync_epoch(void)
{
	return sync_line_last_capture();
}

bool tri_coord_should_follow(uint32_t aa)
{
	ARG_UNUSED(aa);

#if defined(CONFIG_TRI_STRATEGY_HANDOFF)
	/* B: the board that caught it does not follow itself, it only hands off. */
	return false;
#else
	/* A (self-follow + handoff to the other two) and C (redundancy, host dedups): the catching board follows too. */
	return true;
#endif
}

void tri_coord_get_stats(struct tri_coord_stats *out)
{
	if (out != NULL) {
		*out = g_stats;
		out->tx_dropped = peer_txq_dropped(&g_txq);
	}
}
