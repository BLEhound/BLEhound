/*
 * Pure parsing implementation of the LL control PDUs added in BLE 6.x. See ble_ctrl_pdu.h.
 *
 * Reference: Bluetooth Core Specification v6.3, Vol 6, Part B, §2.4.2.
 */

#include "ble_ctrl_pdu.h"

#define UNIT_125_US   125u

/* ECV (Extended ConnInterval Values) valid range: 375µs..4s (Core 6.3 §4.5.1) */
#define CONN_INTERVAL_MIN_US   375u
#define CONN_INTERVAL_MAX_US   4000000u

/* Frame space upper limit 10ms (Core 6.3 §2.4.2.54) */
#define FRAME_SPACE_MAX_US     10000u

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool ble_parse_conn_rate_ind(const uint8_t *ctrl, uint8_t len,
			     struct ble_conn_rate_ind *out)
{
	/* CtrData 14 bytes → Length (including the 1-byte opcode) ≥ 15. Field order per Figure 2.78:
	 * WinOffset(2) Interval(2) Instant(2) SubrateFactor(2) Latency(2)
	 * ContinuationNumber(2) Timeout(2), all little-endian. */
	if (len < 15u) {
		return false;
	}

	const uint32_t interval_us = (uint32_t)rd16(&ctrl[3]) * UNIT_125_US;

	/* On an unencrypted connection, random bytes with llid=3 may happen to hit 0x3F; use the ECV range to reject false positives */
	if (interval_us < CONN_INTERVAL_MIN_US || interval_us > CONN_INTERVAL_MAX_US) {
		return false;
	}

	out->win_offset_us = (uint32_t)rd16(&ctrl[1]) * UNIT_125_US;
	out->interval_us = interval_us;
	out->instant = rd16(&ctrl[5]);
	out->subrate_factor = rd16(&ctrl[7]);
	out->latency = rd16(&ctrl[9]);
	out->continuation_number = rd16(&ctrl[11]);
	out->timeout_10ms = rd16(&ctrl[13]);
	return true;
}

/* Connection interval (in 1.25ms units) valid range: 6..3200 = 7.5ms..4s (Core §4.5.1) */
#define CONN_INTERVAL_MIN_UNITS 6u
#define CONN_INTERVAL_MAX_UNITS 3200u
#define UNIT_1250_US            1250u

bool ble_parse_conn_update_ind(const uint8_t *ctrl, uint8_t len,
			       struct ble_conn_update_ind *out)
{
	/* CtrData 11 bytes → Length ≥ 12. Field order (§2.4.2.1, all little-endian):
	 * WinSize(1) WinOffset(2) Interval(2) Latency(2) Timeout(2) Instant(2). */
	if (len < 12u) {
		return false;
	}

	/* ctrl[0] is the opcode; CtrData starts at ctrl[1]: WinSize takes 1 byte, followed by the 2-byte fields */
	const uint16_t win_offset = rd16(&ctrl[2]);
	const uint16_t interval = rd16(&ctrl[4]);

	if (interval < CONN_INTERVAL_MIN_UNITS || interval > CONN_INTERVAL_MAX_UNITS ||
	    win_offset > interval) {
		return false;
	}

	out->win_size = ctrl[1];
	out->win_offset = win_offset;
	out->interval = interval;
	out->latency = rd16(&ctrl[6]);
	out->timeout_10ms = rd16(&ctrl[8]);
	out->instant = rd16(&ctrl[10]);
	return true;
}

uint32_t ble_conn_update_new_anchor_us(uint32_t old_anchor_us,
				       const struct ble_conn_update_ind *u)
{
	return old_anchor_us + (uint32_t)u->win_offset * UNIT_1250_US;
}

bool ble_parse_frame_space_rsp(const uint8_t *ctrl, uint8_t len,
			       struct ble_frame_space_rsp *out)
{
	/* CtrData: FS(2) PHYS(1) Spacing_Types(1) → Length ≥ 5 (Figure 2.75) */
	if (len < 5u) {
		return false;
	}

	const uint16_t fs = rd16(&ctrl[1]);

	if (fs == 0u || fs > FRAME_SPACE_MAX_US) {
		return false;
	}

	out->fs_us = fs;
	out->phys = ctrl[3];
	out->spacing_types = ctrl[4];
	return true;
}

const char *ble_ll_ctrl_name(uint8_t opcode)
{
	switch (opcode) {
	case BLE_LL_FEATURE_EXT_REQ:     return "LL_FEATURE_EXT_REQ";
	case BLE_LL_FEATURE_EXT_RSP:     return "LL_FEATURE_EXT_RSP";
	case BLE_LL_CS_SEC_RSP:          return "LL_CS_SEC_RSP";
	case BLE_LL_CS_CAPABILITIES_REQ: return "LL_CS_CAPABILITIES_REQ";
	case BLE_LL_CS_CAPABILITIES_RSP: return "LL_CS_CAPABILITIES_RSP";
	case BLE_LL_CS_CONFIG_REQ:       return "LL_CS_CONFIG_REQ";
	case BLE_LL_CS_CONFIG_RSP:       return "LL_CS_CONFIG_RSP";
	case BLE_LL_CS_REQ:              return "LL_CS_REQ";
	case BLE_LL_CS_RSP:              return "LL_CS_RSP";
	case BLE_LL_CS_IND:              return "LL_CS_IND";
	case BLE_LL_CS_TERMINATE_REQ:    return "LL_CS_TERMINATE_REQ";
	case BLE_LL_CS_FAE_REQ:          return "LL_CS_FAE_REQ";
	case BLE_LL_CS_FAE_RSP:          return "LL_CS_FAE_RSP";
	case BLE_LL_CS_CHANNEL_MAP_IND:  return "LL_CS_CHANNEL_MAP_IND";
	case BLE_LL_CS_SEC_REQ:          return "LL_CS_SEC_REQ";
	case BLE_LL_CS_TERMINATE_RSP:    return "LL_CS_TERMINATE_RSP";
	case BLE_LL_FRAME_SPACE_REQ:     return "LL_FRAME_SPACE_REQ";
	case BLE_LL_FRAME_SPACE_RSP:     return "LL_FRAME_SPACE_RSP";
	case BLE_LL_OTA_UTP_IND:         return "LL_OTA_UTP_IND";
	case BLE_LL_CONNECTION_RATE_REQ: return "LL_CONNECTION_RATE_REQ";
	case BLE_LL_CONNECTION_RATE_IND: return "LL_CONNECTION_RATE_IND";
	default:                         return (const char *)0;
	}
}
