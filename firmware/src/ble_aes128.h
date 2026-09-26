/*
 * AES-128 single-block encryption (one ECB block) -- the primitive under the BLE security functions ah()/e().
 *
 * Pure software, no hardware dependency: the nRF54L has no classic ECB peripheral (only CRACEN), and we
 * only compute this once "when a new RPA shows up", so the throughput requirement is tiny and pulling in
 * the whole PSA/nrf_security stack is not worth it. The same code is unit-tested on the host
 * (test/host/test_rpa.c in the internal repo) and is byte-for-byte consistent with the libblehound implementation.
 *
 * Reference: FIPS-197. Key/plaintext/ciphertext all use the FIPS byte order (big-endian, i.e. the MSO of
 * e(k, p) in the BT spec comes first).
 */
#ifndef BLE_AES128_H_
#define BLE_AES128_H_

#include <stdint.h>

/** out = AES-128_encrypt(key, in); all three are 16 bytes in FIPS order; in/out may alias. */
void ble_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

#endif /* BLE_AES128_H_ */
