/*
 * Tri-board coordination layer implementation — see tri_coord.h. Platform-independent, only calls sync_line / peer_link / peer_msg.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "tri_coord.h"
#include "sync_line.h"
#include "peer_link.h"
#include "peer_msg.h"
#include "peer_txq.h"
#include "conn_follower.h"

LOG_MODULE_REGISTER(tri_coord, LOG_LEVEL_INF);

#define ADV_PDU_CONNECT_IND 0x05
#define SYNC_HEARTBEAT_US   1000000U   /* board 0 periodic time-base heartbeat: once per second (§3.3) */

static uint8_t g_board_id;
static struct tri_coord_stats g_stats;
static uint32_t g_last_heartbeat_us;

/* Inter-board message sending goes through "ISR enqueue → system work-queue thread sends SPI": under peer_link_send is a synchronous spi_write
 * (which k_sem_take-waits for the transfer to complete), illegal to call from the radio receive interrupt, and it would also delay the follower's
 * slot-setup / scheduling timing. The ISR only does encode + enqueue + k_work_submit (ISR-safe), a few microseconds. */
static struct peer_txq g_txq;
static struct k_work g_peer_tx_work;

static void peer_tx_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	uint8_t buf[PEER_MSG_MAX];
	size_t n;

	while ((n = peer_txq_pop(&g_txq, buf, sizeof(buf))) > 0) {
		if (peer_link_send(buf, n) == 0 && buf[0] == PEER_MSG_HANDOFF) {
			g_stats.handoffs_sent++;
		}
	}
}

/* ISR context: enqueue and wake the send thread. Drop when the queue is full (counted into stats, visible in the stats line). */
static void peer_enqueue(const uint8_t *msg, size_t len)
{
	if (peer_txq_push(&g_txq, msg, len)) {
		k_work_submit(&g_peer_tx_work);
	}
}

/* Outgoing-link neighbor (handoff target): A→B→C→A, i.e. (id+1)%3 — consistent with the inter-board SPI master-port link (§4.2). */
static inline uint8_t out_neighbor(uint8_t id)
{
	return (uint8_t)((id + 1U) % 3U);
}

static bool is_adv_channel(uint8_t ch)
{
	return ch >= BLE_CHANNEL_ADV_37;
}

/* Parse the AA from a CONNECT_IND packet (same offset as conn_follower start_following) */
static uint32_t parse_connect_aa(const struct radio_packet *pkt)
{
	const uint8_t *ll = &pkt->pdu[2 + 12];   /* header/len(2) + InitA(6) + AdvA(6) */

	return (uint32_t)ll[0] | ((uint32_t)ll[1] << 8) |
	       ((uint32_t)ll[2] << 16) | ((uint32_t)ll[3] << 24);
}

#if defined(CONFIG_TRI_STRATEGY_HANDOFF) || defined(CONFIG_TRI_STRATEGY_DEDUP)
/* Build one handoff (used by strategy B/C). Parameters taken from the CONNECT_IND.
 * ⚠️ Verify on hardware: anchor_sync_tick here is approximated by the "most recent SYNC tick"; the exact conversion of the event0 anchor into the common
 *   time base needs on-board integration (mapping the CONNECT_IND receive instant + txWinDelay + WinOffset into the SYNC domain). */
static void build_handoff(const struct radio_packet *pkt, struct peer_handoff *ho)
{
	const uint8_t *ll = &pkt->pdu[2 + 12];

	*ho = (struct peer_handoff){0};
	ho->aa = parse_connect_aa(pkt);
	ho->crc_init = (uint32_t)ll[4] | ((uint32_t)ll[5] << 8) | ((uint32_t)ll[6] << 16);
	ho->win_size = ll[7];
	ho->win_offset = (uint16_t)ll[8] | ((uint16_t)ll[9] << 8);
	ho->interval = (uint16_t)ll[10] | ((uint16_t)ll[11] << 8);
	ho->latency = (uint16_t)ll[12] | ((uint16_t)ll[13] << 8);
	ho->timeout = (uint16_t)ll[14] | ((uint16_t)ll[15] << 8);
	ho->ch_map_lo = (uint32_t)ll[16] | ((uint32_t)ll[17] << 8) |
			((uint32_t)ll[18] << 16) | ((uint32_t)ll[19] << 24);
	ho->ch_map_hi = ll[20] & 0x1F;
	ho->hop = ll[21] & 0x1F;
	ho->sca = (ll[21] >> 5) & 0x07;
	ho->event_counter = 0;
	ho->anchor_sync_tick = sync_line_last_capture();
}
#endif /* strategy B/C */

/* Received a message from another board (REQ-triggered master-port read callback). */
static void on_peer_recv(const uint8_t *msg, size_t len)
{
	struct peer_hit hit;
	struct peer_handoff ho;
	const int type = peer_msg_decode(msg, len, &hit, &ho);

	if (type == PEER_MSG_HIT) {
		g_stats.peer_hits++;
	} else if (type == PEER_MSG_HANDOFF) {
		g_stats.handoffs_recv++;
		/* ⚠️ Verify on hardware: here ho should be converted into this board's time base and injected into conn_follower to take over following
		 *   (the "another board takes over" of strategy B/C). Taking over following is on-board bring-up, so for now just record it without injecting. */
	}
}

void tri_coord_init(uint8_t board_id)
{
	g_board_id = board_id;

	(void)sync_line_init(NULL);          /* captured ticks are stored inside sync_line; periodic / frame header read them */
	(void)peer_link_init(on_peer_recv);
	peer_txq_init(&g_txq);
	k_work_init(&g_peer_tx_work, peer_tx_work_fn);
	conn_follower_set_claim_gate(tri_coord_should_follow);

	LOG_INF("tri-board coordination ready: board_id=%u outgoing-link neighbor=%u", board_id, out_neighbor(board_id));
}

void tri_coord_on_packet(const struct radio_packet *pkt)
{
	/* Only handle CRC-valid CONNECT_INDs on this board's advertising channel (anchor hit). */
	if (!pkt->crc_ok || !is_adv_channel(pkt->channel)) {
		return;
	}
	if (pkt->pdu_len < 2 + 22 || (pkt->pdu[0] & 0x0F) != ADV_PDU_CONNECT_IND) {
		return;
	}

	const uint32_t aa = parse_connect_aa(pkt);

	g_stats.own_hits++;

	/* P2: pull a SYNC edge to give the three boards a common time-base anchor (this board captures it too). */
	sync_line_emit();
	g_stats.sync_emits++;

	/* P3: broadcast a hit notification (sent by all strategies, so the host can sense the time base / do dedup). */
	uint8_t buf[PEER_MSG_MAX];
	const struct peer_hit hit = {
		.board_id = g_board_id,
		.aa = aa,
		.anchor_sync_tick = sync_line_last_capture(),
	};
	size_t n = peer_msg_encode_hit(&hit, buf, sizeof(buf));

	if (n > 0) {
		peer_enqueue(buf, n);
	}

	/* Strategy B/C: also hand the connection parameters off to the outgoing-link neighbor (taking over following is on-board bring-up).
	 * handoffs_sent is counted only after the send thread actually sends it. */
#if defined(CONFIG_TRI_STRATEGY_HANDOFF) || defined(CONFIG_TRI_STRATEGY_DEDUP)
	struct peer_handoff ho;

	build_handoff(pkt, &ho);
	n = peer_msg_encode_handoff(&ho, buf, sizeof(buf));
	if (n > 0) {
		peer_enqueue(buf, n);
	}
#endif
}

void tri_coord_periodic(void)
{
	/* Only let board 0 send the periodic time-base heartbeat, to avoid all three boards driving the open-drain line at once and overlapping edges.
	 * On a hit, any board still emits its own edge (see on_packet) — "whoever captures emits" is not limited by this. */
	if (g_board_id != 0) {
		return;
	}

	const uint32_t now = radio_now_us();

	if ((uint32_t)(now - g_last_heartbeat_us) < SYNC_HEARTBEAT_US) {
		return;
	}
	g_last_heartbeat_us = now;

	sync_line_emit();
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
	/* B: the capturing board does not follow itself, but hands off to the outgoing-link neighbor (the neighbor takes over after receiving the handoff, on-board bring-up). */
	return false;
#else
	/* A (self-follow) and C (redundant: multiple boards follow the same one + host dedup): the capturing board always follows itself. */
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
