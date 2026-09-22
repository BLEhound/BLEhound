/*
 * Pure parsing of the LL control PDUs added in BLE 6.x (no hardware dependency)
 *
 * Splits "parsing CtrData per the Core Spec byte layout" out of conn_follower's hardware/scheduling
 * logic -- pure functions, no side effects, can be rigorously self-tested on a PC with byte vectors
 * built from the spec on the host. If the byte offset of these PDUs is off by
 * even one, following silently loses the connection, so like CSA they must be thoroughly tested
 * before running on the board.
 *
 * Reference: Bluetooth Core Specification v6.3, Vol 6, Part B, §2.4.2 and Table 2.22.
 *
 * Also covers the parsing of an older PDU where "one wrong offset loses the connection":
 * LL_CONNECTION_UPDATE_IND (§2.4.2.1).
 */

#ifndef BLE_CTRL_PDU_H_
#define BLE_CTRL_PDU_H_

#include <stdbool.h>
#include <stdint.h>

/* ---- LL control PDU opcodes added in 6.x (Core v6.3 Vol6 PartB Table 2.22) ---- */
#define BLE_LL_FEATURE_EXT_REQ       0x2B  /* Extended feature set (6.0) */
#define BLE_LL_FEATURE_EXT_RSP       0x2C
#define BLE_LL_CS_SEC_RSP            0x2D  /* Channel Sounding negotiation family (6.0), 0x2D..0x3A */
#define BLE_LL_CS_CAPABILITIES_REQ   0x2E
#define BLE_LL_CS_CAPABILITIES_RSP   0x2F
#define BLE_LL_CS_CONFIG_REQ         0x30
#define BLE_LL_CS_CONFIG_RSP         0x31
#define BLE_LL_CS_REQ                0x32
#define BLE_LL_CS_RSP                0x33
#define BLE_LL_CS_IND                0x34
#define BLE_LL_CS_TERMINATE_REQ      0x35
#define BLE_LL_CS_FAE_REQ            0x36
#define BLE_LL_CS_FAE_RSP            0x37
#define BLE_LL_CS_CHANNEL_MAP_IND    0x38
#define BLE_LL_CS_SEC_REQ            0x39
#define BLE_LL_CS_TERMINATE_RSP      0x3A
#define BLE_LL_FRAME_SPACE_REQ       0x3B  /* Frame space negotiation (6.0) */
#define BLE_LL_FRAME_SPACE_RSP       0x3C
#define BLE_LL_OTA_UTP_IND           0x3D  /* LE test mode OTA (6.2) */
#define BLE_LL_CONNECTION_RATE_REQ   0x3E  /* Short connection interval (6.2) */
#define BLE_LL_CONNECTION_RATE_IND   0x3F

/** Contiguous opcode range of the CS negotiation PDUs */
#define BLE_LL_CS_OPCODE_MIN         BLE_LL_CS_SEC_RSP        /* 0x2D */
#define BLE_LL_CS_OPCODE_MAX         BLE_LL_CS_TERMINATE_RSP  /* 0x3A */

/** Whether the opcode belongs to the Channel Sounding negotiation family (6.0) */
static inline bool ble_ll_ctrl_is_cs(uint8_t opcode)
{
	return opcode >= BLE_LL_CS_OPCODE_MIN && opcode <= BLE_LL_CS_OPCODE_MAX;
}

/**
 * Parsed result of LL_CONNECTION_RATE_IND (0x3F, 6.2 short connection interval).
 * The interval is in units of **125µs** (not the older 1.25ms); it has already been converted to µs here.
 */
struct ble_conn_rate_ind {
	uint32_t win_offset_us;      /* WinOffset × 125µs */
	uint32_t interval_us;        /* Interval × 125µs; ECV range 375µs..4s */
	uint16_t instant;            /* connection event at which it takes effect */
	uint16_t subrate_factor;     /* connSubrateFactor */
	uint16_t latency;            /* connPeripheralLatency */
	uint16_t continuation_number;/* connContinuationNumber */
	uint16_t timeout_10ms;       /* connSupervisionTimeout ÷ 10ms */
};

/**
 * Parse the CtrData of LL_CONNECTION_RATE_IND (excluding the opcode).
 *
 * @param ctrl  points at the opcode byte (ctrl[0]==0x3F), followed by 14 bytes of CtrData
 * @param len   the Length field of the LL control PDU (= 1 + CtrData length)
 * @param out   parsed result (valid only when true is returned)
 * @return true if the length is sufficient and the interval falls within the valid ECV range (375µs..4s); otherwise false
 */
bool ble_parse_conn_rate_ind(const uint8_t *ctrl, uint8_t len,
			     struct ble_conn_rate_ind *out);

/**
 * Parsed result of LL_CONNECTION_UPDATE_IND (0x00, Core Vol 6 Part B §2.4.2.1).
 * The time fields are all in 1.25ms units (same as CONNECT_IND); kept as-is here, to be converted by the caller.
 */
struct ble_conn_update_ind {
	uint8_t win_size;        /* transmitWindowSize, ×1.25ms */
	uint16_t win_offset;     /* transmitWindowOffset, ×1.25ms; 0 ≤ WinOffset ≤ Interval */
	uint16_t interval;       /* connInterval, ×1.25ms; 6..3200 */
	uint16_t latency;        /* connPeripheralLatency */
	uint16_t timeout_10ms;   /* connSupervisionTimeout ÷ 10ms */
	uint16_t instant;        /* connection event at which it takes effect */
};

/**
 * Parse the CtrData of LL_CONNECTION_UPDATE_IND (excluding the opcode).
 *
 * @param ctrl  points at the opcode byte (ctrl[0]==0x00), followed by 11 bytes of CtrData
 * @param len   the Length field of the LL control PDU (= 1 + CtrData length)
 * @param out   parsed result (valid only when true is returned)
 * @return true if the length is sufficient, Interval is within 7.5ms..4s, and WinOffset ≤ Interval; otherwise false
 */
bool ble_parse_conn_update_ind(const uint8_t *ctrl, uint8_t len,
			       struct ble_conn_update_ind *out);

/**
 * The new anchor point of the event at which the update takes effect (instant) (Core Vol 6 Part B §5.1.1):
 * the central's first packet lands within a window starting at "old anchor + transmitWindowOffset" and
 * transmitWindowSize wide.
 * This step was not done before: when WinOffset is large (32 × 1.25ms = 40ms on a real device), the follower
 * opens its window at the old anchor and inevitably loses the connection.
 *
 * @param old_anchor_us  the anchor point extrapolated to the instant event by the old interval (µs, 32-bit wraparound)
 * @return the new anchor point (window start), µs, 32-bit wraparound
 */
uint32_t ble_conn_update_new_anchor_us(uint32_t old_anchor_us,
				       const struct ble_conn_update_ind *u);

/** Parsed result of LL_FRAME_SPACE_RSP (0x3C, 6.0 frame space negotiation) */
struct ble_frame_space_rsp {
	uint16_t fs_us;         /* the selected frame space, µs (≤10ms) */
	uint8_t phys;           /* applicable PHY bitmap */
	uint8_t spacing_types;  /* applicable spacing-type bitmap: bit0=T_IFS_ACL_CP bit1=T_IFS_ACL_PC… */
};

/** Two-bit mask for the ACL-direction T_IFS in spacing_types */
#define BLE_FS_SPACING_ACL_MASK  0x03u

/**
 * Parse the CtrData of LL_FRAME_SPACE_RSP.
 *
 * @return true if the length is sufficient and the frame space is within the valid range (0<fs≤10ms); otherwise false
 */
bool ble_parse_frame_space_rsp(const uint8_t *ctrl, uint8_t len,
			       struct ble_frame_space_rsp *out);

/** opcode → human-readable name (returns NULL if unknown). For logs / capture summaries. */
const char *ble_ll_ctrl_name(uint8_t opcode);

#endif /* BLE_CTRL_PDU_H_ */
