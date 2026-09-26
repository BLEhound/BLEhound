/*
 * Advertising PDU target filter -- pure function, no hardware dependency, called from
 * main's packet-RX interrupt and unit-tested on the host.
 *
 * Single-target mode: once a target MAC is set, of the PDUs received on the advertising
 * AA only those "related to the target" are forwarded to the host --
 *   - Sent by the target itself: ADV_IND / ADV_DIRECT_IND / ADV_NONCONN_IND / ADV_SCAN_IND / SCAN_RSP
 *     (AdvA at payload offset 0);
 *   - Sent to the target by others: SCAN_REQ / CONNECT_IND (ScanA/InitA first, AdvA at offset 6) --
 *     kept so you can see who is scanning it and who is connecting to it;
 *   - Extended advertising (ADV_EXT_IND / AUX_* share type 0x07, AUX_CONNECT_RSP 0x08): counts only
 *     when the extended header carries AdvA and it matches. The primary-channel ADV_EXT_IND often
 *     carries no AdvA; this version treats it as "unrelated".
 * CRC-bad packets are matched against AdvA by the same rules: kept if they match, which gives the
 * "include packets with CRC errors" option meaning.
 *
 * Reference: BT Core Spec Vol 6, Part B, §2.3 (advertising PDU format), §2.3.4 (extended header).
 */

#ifndef ADV_FILTER_H_
#define ADV_FILTER_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ADV_FILTER_PDU_ADV_IND         0x00
#define ADV_FILTER_PDU_ADV_DIRECT_IND  0x01
#define ADV_FILTER_PDU_ADV_NONCONN_IND 0x02
#define ADV_FILTER_PDU_SCAN_REQ        0x03
#define ADV_FILTER_PDU_SCAN_RSP        0x04
#define ADV_FILTER_PDU_CONNECT_IND     0x05
#define ADV_FILTER_PDU_ADV_SCAN_IND    0x06
#define ADV_FILTER_PDU_ADV_EXT_IND     0x07
#define ADV_FILTER_PDU_AUX_CONNECT_RSP 0x08

#define ADV_FILTER_EXT_FLAG_ADVA       0x01

/**
 * Extract the AdvA ("who this packet is about") from an LL PDU (including the 2-byte header) received on the advertising AA.
 * @param pdu     Complete LL PDU: header(1) + length(1) + payload
 * @param pdu_len Total PDU length
 * @return pointer to the 6-byte AdvA inside the PDU (over-the-air little-endian); NULL if the type is unknown, carries no AdvA, or is too short
 */
static inline const uint8_t *adv_pdu_adva(const uint8_t *pdu, uint16_t pdu_len)
{
	if (pdu == NULL || pdu_len < 2) {
		return NULL;
	}

	const uint8_t type = pdu[0] & 0x0F;

	switch (type) {
	case ADV_FILTER_PDU_ADV_IND:
	case ADV_FILTER_PDU_ADV_DIRECT_IND:
	case ADV_FILTER_PDU_ADV_NONCONN_IND:
	case ADV_FILTER_PDU_SCAN_RSP:
	case ADV_FILTER_PDU_ADV_SCAN_IND:
		return (pdu_len >= 2 + 6) ? &pdu[2] : NULL;
	case ADV_FILTER_PDU_SCAN_REQ:
	case ADV_FILTER_PDU_CONNECT_IND:
		return (pdu_len >= 2 + 12) ? &pdu[2 + 6] : NULL;
	case ADV_FILTER_PDU_ADV_EXT_IND:
	case ADV_FILTER_PDU_AUX_CONNECT_RSP:
		/* pdu[2] = ExtHdrLen (low 6 bits) | AdvMode (high 2 bits); pdu[3] = extended header Flags;
		 * AdvA is the first optional field after Flags (bit0). */
		if (pdu_len >= 2 + 1 + 1 + 6) {
			const uint8_t ext_len = pdu[2] & 0x3F;
			const uint8_t flags = pdu[3];

			if (ext_len >= 1 + 6 && (flags & ADV_FILTER_EXT_FLAG_ADVA) != 0) {
				return &pdu[4];
			}
		}
		return NULL;
	default:
		return NULL;
	}
}

/**
 * Whether an LL PDU (including the 2-byte header) received on the advertising AA is related to the target AdvA.
 * @param target  6-byte BLE address, little-endian (matching the over-the-air order)
 */
static inline bool adv_pdu_matches_target(const uint8_t *pdu, uint16_t pdu_len,
					  const uint8_t target[6])
{
	const uint8_t *adva = adv_pdu_adva(pdu, pdu_len);

	return adva != NULL && memcmp(adva, target, 6) == 0;
}

/**
 * Parse "AA:BB:CC:DD:EE:FF" (also accepts '-' separators, any case) into a 6-byte
 * **over-the-air little-endian** address (display order is most-significant-first, the
 * over-the-air order is reversed, matching HOST_CMD_SET_TARGET / adv_pdu_matches_target).
 * @return true if the format is valid
 */
static inline bool adv_filter_parse_mac(const char *text, uint8_t out[6])
{
	if (text == NULL) {
		return false;
	}
	uint8_t disp[6];
	int n = 0;

	while (*text != '\0' && n < 6) {
		uint8_t v = 0;

		for (int k = 0; k < 2; k++) {
			const char c = *text++;
			uint8_t d;

			if (c >= '0' && c <= '9') {
				d = (uint8_t)(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				d = (uint8_t)(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				d = (uint8_t)(c - 'A' + 10);
			} else {
				return false;
			}
			v = (uint8_t)((v << 4) | d);
		}
		disp[n++] = v;
		if (*text == ':' || *text == '-') {
			text++;
		}
	}
	if (n != 6 || *text != '\0') {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		out[i] = disp[5 - i];
	}
	return true;
}

#endif /* ADV_FILTER_H_ */
