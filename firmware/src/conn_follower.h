/*
 * Connection Follower — the core capability of the sniffer.
 *
 * Once a CONNECT_IND is captured, parse the connection parameters, use TIMER1 to precisely
 * schedule each connection event, and follow the connection along the hop sequence computed
 * by CSA #1/#2, continuously capturing packets on the data channels.
 *
 * Encryption has no effect on following: it only changes the PDU's payload + MIC, not the
 * access address, LL header, or hop pattern — so we keep following after encryption starts,
 * capturing ciphertext (decryption is left to the host).
 *
 * All logic runs in interrupt context (RX interrupt + TIMER1 scheduling interrupt), never in
 * a thread — the timing requirements of connection following are at the µs level and cannot
 * tolerate the jitter of thread scheduling.
 *
 * Reference: Bluetooth Core Spec v5.2, Vol 6, Part B, §4.5 (connection), §2.3.3 (CONNECT_IND).
 */

#ifndef CONN_FOLLOWER_H_
#define CONN_FOLLOWER_H_

#include <stdbool.h>
#include <stdint.h>

#include "radio_hal.h"

/** Upper bound on the number of connections/streams tracked at once. A single radio follows
 *  several via time-division multiplexing; the more there are, the more often they collide.
 *  Each BIS in a BIG occupies its own slot, so keep this more generous than the "connection count". */
#define CONN_FOLLOW_MAX_SLOTS 6

/** Maximum number of BIS streams to follow in parallel within one BIG — don't let one BIG eat all the slots. */
#define BIS_MAX_STREAMS 4

/** Connection-following statistics, for the host/logs to see the run status */
struct conn_follower_stats {
	uint32_t connects_seen;    /**< Number of CONNECT_INDs captured */
	uint32_t conn_events;      /**< Number of connection events successfully followed (master packet received) */
	uint32_t events_missed;    /**< Cumulative empty events (no packet received) */
	uint32_t data_packets;     /**< Number of CRC-correct data packets captured while following */
	uint32_t map_updates;      /**< Number of channel map updates processed */
	uint32_t conn_updates;     /**< Number of connection parameter updates processed */
	uint32_t phy_updates;      /**< Number of PHY updates processed (switch to 2M/Coded) */
	uint32_t lost;             /**< Number of times a slot was released after being deemed lost on timeout */
	uint32_t collisions;       /**< Number of times an event was missed because the radio was busy with another connection */
	uint32_t injects_followed; /**< Tri-device co-follow: connections successfully taken over via host relay (HOST_CMD_FOLLOW) */
	uint32_t relock_recovered; /**< M4 relock: number of times re-acquired after loss by widening the window and re-searching */
	uint32_t hints_applied;    /**< Key hints: encrypted control PDUs the host decrypted and relayed (registered on a slot) */
	uint32_t unmapped_events;  /**< Connection events served in unmapped-channel guard mode (encrypted, channel map untrusted) */
	uint8_t active_now;        /**< Number of connections currently being tracked */
	uint8_t peak_concurrent;   /**< Maximum number of connections ever tracked simultaneously */
	uint32_t ext_adv_chase;    /**< Extended advertising: number of times a secondary (AUX) channel was successfully chased */
	uint32_t ext_adv_throttled;/**< Extended advertising: number of chases abandoned because the time budget ran out */
	uint32_t periodic_synced;  /**< Periodic advertising: number of successful syncs (tracking established) */
	uint32_t bis_synced;       /**< BIS (LE Audio broadcast isochronous stream): times BIGInfo was detected and tracking established */
	uint32_t cis_synced;       /**< CIS (LE Audio connected isochronous stream): times tracking was established from LL_CIS_IND */
	uint32_t subrate_updates;  /**< Number of LL_SUBRATE_INDs processed (connection subrating, 5.3) */
	uint32_t pawr_responses;   /**< PAwR: number of times a packet was captured in a response slot (5.4) */
	uint32_t aux_connects;     /**< Connections followed that were established via AUX_CONNECT_REQ (extended advertising connection, 5.0) */
	/* ---- 6.x new control PDUs ---- */
	uint32_t cs_pdus;          /**< Total Channel Sounding negotiation PDUs captured (LL_CS_*, 6.0) */
	uint32_t frame_space_updates;/**< Number of LL_FRAME_SPACE_RSPs processed (frame-space negotiation, 6.0) */
	uint32_t conn_rate_updates;/**< Number of LL_CONNECTION_RATE_INDs processed (short connection interval, 6.2) */
	uint32_t feature_ext_pdus; /**< Number of LL_FEATURE_EXT_REQ/RSP captured (extended feature set, 6.0) */
	uint32_t adv_decision;     /**< Number of ADV_DECISION_INDs captured (decision-based advertising filtering, 6.0) */
	/* ---- Scheduling instrumentation: when a subevent's budget is only ~200µs, these two decide whether we catch it ---- */
	uint32_t open_late_max_us; /**< Maximum lateness of serve_open relative to the scheduled window-open time */
	uint32_t open_cost_max_us; /**< Maximum time to configure the radio (switch channel/AA/CRC + ramp-up) */
	uint32_t open_cost_sum_us; /**< Same as above, accumulated; combine with open_count for the average */
	uint32_t open_count;       /**< Number of serve_open calls */
};

/** Snapshot of a single tracked connection (for evaluation) */
struct conn_slot_info {
	bool active;
	uint32_t access_addr;
	uint32_t interval_us;
	uint16_t event_counter;
	uint32_t events;           /**< Number of events this connection successfully followed */
	uint32_t serves;           /**< Number of times this slot was actually served (RX window opened) */
	uint32_t data_packets;     /**< Number of CRC-correct data packets captured on this connection */
	uint32_t crc_errors;       /**< Packets where AA matched but CRC failed (>0 = channel/timing were right) */
	int32_t anchor_delta_us;   /**< Last RX timestamp − predicted anchor, µs (measures anchor accuracy) */
	/* ---- ISO-slot specific (BIS/CIS; bis_nse==0 means not an ISO slot) ---- */
	uint8_t bis_nse;           /**< Number of subevents per event */
	uint8_t bis_bn;            /**< Burst Number: number of new payloads per event */
	uint8_t bis_num;           /**< Number of BIS streams in this BIG (0 for CIS) */
	uint8_t bis_index;         /**< Which BIS this slot follows (1-based; 0 for CIS) */
	uint8_t bis_ev_hits_last;  /**< Number of subevents captured in the last complete event (/NSE = full-capture rate) */
	uint32_t bis_sub_interval_us; /**< Interval between adjacent subevents, µs */
	uint32_t bis_slot_spacing_us; /**< Interval between two adjacent on-air packets (used to size the window), µs */
	uint32_t bis_pdu_air_us;   /**< On-air duration of one subevent, µs */
	bool bis_locked;           /**< Anchored on a real ISO packet? (not locked = still searching with a wide window) */
	bool pawr;                 /**< This slot is a PAwR (5.4) periodic train; subevent parameters come from ACAD 0x32 */
	uint16_t miss_count;       /**< Current number of consecutive misses */
	/* ---- Connection subrating (subrating, 5.3); subrate_factor<=1 means disabled ---- */
	uint16_t subrate_factor;   /**< Subrate factor S */
	uint16_t subrate_cont;     /**< continuationNumber */
	uint16_t timeout_10ms;     /**< Supervision timeout, ×10ms */
	bool encrypted;
	uint8_t phy;               /**< PHY currently being followed: 0=1M 1=2M 2/3=Coded */
	uint16_t frame_space_us;   /**< This link's T_IFS after 6.0 frame-space negotiation, µs (0=not negotiated, default 150) */
};

/** Read info for slot idx (idx < CONN_FOLLOW_MAX_SLOTS). */
void conn_follower_get_slot(uint8_t idx, struct conn_slot_info *out);

/**
 * Initialize and enter scanning state. Call after radio_init().
 * @param scan_channel The advertising channel to dwell on while scanning (37/38/39)
 */
void conn_follower_init(uint8_t scan_channel);

/**
 * Called for every received packet (in the radio RX interrupt).
 * In scanning state, identifies CONNECT_IND to trigger following; in following state, used for
 * resynchronization and LL control packet parsing.
 */
void conn_follower_on_packet(const struct radio_packet *pkt);

/** Set the scan channel. When called while following, it is only recorded and takes effect on return to scanning. */
void conn_follower_set_scan_channel(uint8_t ch);

/**
 * Set the target device filter: only follow connections to this MAC.
 * @param mac  6-byte BLE address (little-endian, as on air); pass NULL to clear the filter (follow any connection)
 */
void conn_follower_set_target(const uint8_t *mac);

/** Enable/disable three-channel round-robin scanning (on by default). When off, dwells only on the channel set by set_scan_channel. */
void conn_follower_set_scan_hopping(bool enable);

/**
 * Claim gate on a hit (for tri-device coordination). After a CONNECT_IND is captured and its AA parsed,
 * and **before** building a slot to follow it, if a gate function is registered it is asked first
 * "is this one mine to follow?"; returning false means no slot is built (left to another device / deduplication).
 * The default (unregistered, gate=NULL) behaves exactly like a single board — always follow. The gate is called
 * in RX interrupt context and must be extremely short.
 */
typedef bool (*conn_claim_gate_t)(uint32_t aa);
void conn_follower_set_claim_gate(conn_claim_gate_t gate);

/**
 * Tri-device co-follow: a connection's parameters relayed from the host, so this device also takes over
 * following (without having to capture the CONNECT_IND itself).
 * anchor0_us is the event0 anchor in **this device's** TIMER time base, in µs (the host has already converted it
 * by the inter-board SYNC offset).
 * May be called in CDC interrupt context: it only posts the parameters into a mailbox; the actual slot build
 * happens in the radio interrupt (to avoid contending for slots with the RX interrupt).
 */
struct conn_follow_inject {
	uint32_t aa;
	uint32_t crc_init;
	uint8_t  chan_map[5];
	uint8_t  hop;
	uint8_t  csa2;
	uint16_t interval;
	uint16_t latency;
	uint16_t timeout;
	uint16_t win_size;
	uint32_t anchor0_us;
};
void conn_follower_request_inject(const struct conn_follow_inject *p);

/**
 * Key hint (HOST_CMD_LL_CTRL_HINT): an encrypted LL control PDU the host decrypted with the LTK.
 * May be called from the CDC interrupt context: it only posts to a mailbox; the actual registration
 * on the slot happens in the radio interrupt (poll_hint).
 * pdu = plaintext LL data PDU (2-byte header + payload, MIC stripped).
 */
#define CONN_FOLLOW_HINT_MAX 40
struct conn_follow_hint {
	uint32_t aa;
	uint8_t  pdu_len;
	uint8_t  pdu[CONN_FOLLOW_HINT_MAX];
};
void conn_follower_request_hint(const struct conn_follow_hint *h);

/**
 * Consume the mailboxes posted by the host (FOLLOW relay / key hint). This used to happen only in
 * conn_follower_on_packet, but in single-target mode, while the target is not advertising, every
 * received packet is dropped at the filter and the follower is never called, so a FOLLOW was never
 * picked up (real hardware 2026-09-27: in tri-board joint follow only the discovering board followed).
 * Now it is called once whenever the radio interrupt receives any packet (before the filter), and the
 * main loop calls it again with interrupts off as a fallback, so it is consumed even when the air is quiet.
 */
void conn_follower_poll_host_requests(void);

/** Whether currently in following state */
bool conn_follower_is_following(void);

/**
 * Direction of the packet just handed to conn_follower_on_packet (call right after it in the radio interrupt):
 * within the connection event being served, the first packet = central→peripheral (1), the rest =
 * peripheral→central (2); 0 when not serving a connection (advertising, periodic advertising, ISO).
 * If the event's first packet was missed a peripheral packet is mistaken for a central one; the host uses
 * this for display only, following is unaffected.
 */
uint8_t conn_follower_last_packet_direction(void);

/**
 * Number of currently active connection slots — counts only ACL connections built from a self-captured
 * CONNECT_IND / relay takeover, excluding periodic advertising and ISO (BIS/CIS) slots. Readable from thread
 * context (reads a few bools per slot; concurrent with the interrupt it may momentarily differ by at most 1),
 * used for LED indication.
 */
uint8_t conn_follower_active_connections(void);

/** Cumulative CRC-correct packets received on ACL connection slots (excluding periodic advertising/ISO). Thread-readable, used to toggle the LED with RF activity. */
uint32_t conn_follower_acl_packets(void);

/** Number of times an ACL connection slot was released due to **supervision timeout** (excluding a normal disconnect via the peer's LL_TERMINATE_IND). Used for the LED "lost" indication. */
uint32_t conn_follower_acl_lost_timeouts(void);

/**
 * Single-target mode (on by default): once there is an active ACL connection slot, ignore new CONNECT_INDs and
 * relay injections; during event gaps, don't return to the advertising channels to scan and don't chase extended
 * advertising; after the connection ends (peer disconnect / supervision timeout), automatically resume scanning.
 * Turning it off restores the original multi-target evaluation behavior (scan during gaps, follow up to
 * CONN_FOLLOW_MAX_SLOTS connections at once).
 */
void conn_follower_set_single_target(bool enable);

/** Read a statistics snapshot */
void conn_follower_get_stats(struct conn_follower_stats *out);

#endif /* CONN_FOLLOWER_H_ */
