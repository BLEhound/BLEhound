/*
 * Host interface —— sends captured packets to the host (Wireshark extcap) over USB CDC ACM.
 *
 * Wire format: each frame is first assembled per the structure below, then the whole thing is
 * COBS-encoded, with a single 0x00 delimiter appended at the end. The host only needs to split
 * on 0x00 and COBS-decode frame by frame; it re-aligns automatically if it joins mid-stream or
 * drops a byte.
 *
 * Capture frame (firmware → PC), little-endian:
 *
 *   Offset  Len  Field         Description
 *   ------  ---  ------------  --------------------------------------------
 *      0     1  type          HOST_FRAME_PACKET
 *      1     1  flags         see HOST_FLAG_*
 *      2     4  timestamp_us  time the access address was received (µs)
 *      6     1  channel       BLE channel index 0..39
 *      7     1  rssi          int8, dBm (negative)
 *      8     1  phy           0=1M 1=2M 2=Coded S8 3=Coded S2
 *      9     4  access_addr   0x8E89BED6 for advertising, the connection's AA otherwise
 *     13     3  crc           the received 24-bit CRC, raw
 *     16     1  pdu_len       number of PDU bytes that follow (= 2 + payload length)
 *     17     N  pdu           complete LL PDU: header(1) + length(1) + payload
 *
 * The host concatenates access_addr + pdu + crc in that order to form the link-layer frame
 * that Wireshark's LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR expects.
 *
 * Command frame (PC → firmware), also COBS-framed:
 *
 *   Offset  Len  Field         Description
 *   ------  ---  ------------  --------------------------------------------
 *      0     1  type          HOST_CMD_*
 *      1     N  args          command-specific
 */

#ifndef HOST_IFACE_H_
#define HOST_IFACE_H_

#include <stdbool.h>
#include <stdint.h>

#include "radio_hal.h"

/* ---- frame types ---- */
#define HOST_FRAME_PACKET     0x01   /**< capture frame, firmware → PC */

#define HOST_CMD_SET_CHANNEL  0x81   /**< arg: 1 byte, BLE channel index 0..39 */
#define HOST_CMD_START        0x82   /**< no args, start capturing */
#define HOST_CMD_STOP         0x83   /**< no args, stop capturing */
#define HOST_CMD_SET_TARGET   0x84   /**< arg: 6-byte MAC (little-endian); all-zero = clear filter */
#define HOST_CMD_SET_HOPPING  0x85   /**< arg: 1 byte, nonzero enables three-channel round-robin scan */
#define HOST_CMD_FOLLOW       0x86   /**< tri-device co-follow relay: the host hands the connection
                                       *   parameters captured by one device to the other two so they
                                       *   take over following. Args are 25 bytes (little-endian): aa(4)
                                       *   crc_init(3) chan_map(5) hop(1) csa2(1) interval(2) latency(2)
                                       *   timeout(2) win_size(1) anchor0_us(4). anchor0_us is the event0
                                       *   anchor in µs on **this device's** TIMER time base. */
#define HOST_CMD_SET_SINGLE_TARGET 0x87 /**< arg: 1 byte, nonzero = single-target mode (default on): with no target set,
                                          *   only scan advertising and follow no connection; with a target set, follow only
                                          *   that target's connection and stop scanning / accepting new ones once following;
                                          *   0 = multi-target evaluation mode, follow everything (up to 6) */

/* ---- flags bits ---- */
#define HOST_FLAG_CRC_OK      (1U << 0)  /**< CRC check passed */
#define HOST_FLAG_DIR_S2M     (1U << 1)  /**< direction slave→master (reserved for connection following) */
#define HOST_FLAG_ENCRYPTED   (1U << 2)  /**< this packet is encrypted (reserved for phases B/C) */
#define HOST_FLAG_TRI         (1U << 3)  /**< tri-device mode: 5-byte extension after the header (board_id + sync_epoch) */

/** header length (all fixed fields before pdu) */
#define HOST_FRAME_HEADER_LEN 17

/**
 * Tri-device extension length (present only when flags has HOST_FLAG_TRI, immediately after the fixed header):
 *   Offset (from hdr end)  Len   Field
 *   ------------------  ----  ------------------------------------------
 *      0                 1    board_id     this board's id 0/1/2 (design doc §4.5)
 *      1                 4    sync_epoch   this board's TIMER tick at the most recent SYNC edge (µs, little-endian; 0 = none yet)
 * Older boards do not set HOST_FLAG_TRI and pdu still sits at offset HOST_FRAME_HEADER_LEN —— backward compatible.
 */
#define HOST_TRI_EXT_LEN      5

/** Callback invoked when a command is received. Called in CDC interrupt context, so keep it short. */
typedef void (*host_cmd_cb_t)(uint8_t cmd, const uint8_t *args, uint8_t args_len);

/**
 * Initialize CDC ACM and register the command callback.
 *
 * @return 0 on success, negative errno on failure
 */
int host_iface_init(host_cmd_cb_t cmd_cb);

/**
 * Whether the host has opened the serial port (DTR asserted). While it is not open, sends are
 * dropped outright to avoid filling up the TX buffer.
 */
bool host_iface_connected(void);

/**
 * Send one capture frame. Non-blocking: if there is not enough buffer space it drops the frame and
 * counts it, never blocking the caller.
 *
 * @return 0 queued for transmission, -ENOTCONN host not connected, -ENOMEM buffer full (counted)
 */
int host_iface_send_packet(const struct radio_packet *pkt, uint32_t access_addr,
			   uint8_t phy);

/**
 * Send a capture frame with tri-device metadata (HOST_FLAG_TRI): additionally carries board_id and
 * sync_epoch, so the host can tag the source board when merging three streams and align timestamps
 * onto a common time base. Otherwise identical to host_iface_send_packet.
 *
 * @param board_id     this board's id 0/1/2
 * @param sync_epoch   this board's TIMER-time-base tick at the most recent SYNC edge (0 = none yet)
 * @return same as host_iface_send_packet
 */
int host_iface_send_packet_tri(const struct radio_packet *pkt, uint32_t access_addr,
			       uint8_t phy, uint8_t board_id, uint32_t sync_epoch);

/** Number of frames dropped due to insufficient TX buffer space */
uint32_t host_iface_dropped(void);

#endif /* HOST_IFACE_H_ */
