/*
 * BLE Channel Selection Algorithm #1 / #2
 *
 * The core of connection following: the central and peripheral use the same algorithm on every
 * connection event to compute the data channel for the next hop. Only if the sniffer computes this
 * algorithm **exactly identically** to them can it hop along and capture packets.
 *
 * The algorithm itself is pure computation (no side effects, no hardware dependency), so it can be
 * unit-tested for equivalence against the official implementation on the host.
 *
 * Reference: Bluetooth Core Spec v5.2, Vol 6, Part B, §4.5.8.
 */

#ifndef BLE_CSA_H_
#define BLE_CSA_H_

#include <stdint.h>

/** Total number of data channels (0..36) */
#define BLE_DATA_CHANNELS 37

/**
 * Compute the channel identifier from the access address (used by CSA#2).
 * = AA high 16 bits XOR low 16 bits.
 */
uint16_t ble_csa_channel_id(uint32_t access_addr);

/**
 * CSA#1: compute the data channel for a given connection event.
 *
 * @param last_unmapped  the previous unmapped channel (initial value 0); this function updates it
 * @param hop            hop increment (from CONNECT_IND, 5..16)
 * @param latency        peripheral latency (number of skipped events, usually 0)
 * @param chan_map       37-bit channel map, 5 bytes little-endian (bit i = data channel i available)
 * @param chan_count     number of available channels in chan_map
 * @return the data channel to use for this event, 0..36
 */
uint8_t ble_csa1_next(uint8_t *last_unmapped, uint8_t hop, uint16_t latency,
		      const uint8_t *chan_map, uint8_t chan_count);

/**
 * CSA#2: compute the data channel for a given connection event.
 *
 * @param counter     the connection event counter for this event
 * @param chan_id     the result of ble_csa_channel_id()
 * @param chan_map    37-bit channel map, 5 bytes little-endian
 * @param chan_count  number of available channels
 * @return data channel 0..36
 */
uint8_t ble_csa2_next(uint16_t counter, uint16_t chan_id,
		      const uint8_t *chan_map, uint8_t chan_count);

/**
 * CSA#2 (ISO variant): compute the channel of the **first subevent** in a BIG / CIG event.
 *
 * The channel is the same as the one computed by ble_csa2_next(), but it additionally emits two
 * recursion states, used by ble_csa2_iso_subevent() to continue advancing through the subsequent
 * subevents within the same event.
 *
 * @param counter     BIG event counter (= bisPayloadCount / BN) or CIS event counter
 * @param chan_id     ble_csa_channel_id(the BIS/CIS's own access address)
 * @param chan_map    37-bit channel map, 5 bytes little-endian
 * @param chan_count  number of available channels
 * @param prn_lu      [out] subevent pseudo-random number recursion state
 * @param remap_idx   [out] remap index recursion state
 * @return data channel 0..36
 */
uint8_t ble_csa2_iso_event(uint16_t counter, uint16_t chan_id,
			   const uint8_t *chan_map, uint8_t chan_count,
			   uint16_t *prn_lu, uint16_t *remap_idx);

/**
 * CSA#2 (ISO variant): compute the channel of the **next subevent** within the same event.
 *
 * ble_csa2_iso_event() must have been called for this event first, then this function is called in
 * subevent order; prn_lu / remap_idx are advanced jointly by both and must not skip an index.
 *
 * @param chan_id     as above
 * @param chan_map    37-bit channel map, 5 bytes little-endian
 * @param chan_count  number of available channels
 * @param prn_lu      [in,out] recursion state
 * @param remap_idx   [in,out] recursion state
 * @return data channel 0..36
 */
uint8_t ble_csa2_iso_subevent(uint16_t chan_id, const uint8_t *chan_map,
			      uint8_t chan_count, uint16_t *prn_lu,
			      uint16_t *remap_idx);

/** Count the number of available channels in the 37-bit channel map */
uint8_t ble_csa_channel_count(const uint8_t *chan_map);

#endif /* BLE_CSA_H_ */
