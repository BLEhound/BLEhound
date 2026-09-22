/*
 * COBS (Consistent Overhead Byte Stuffing) encode/decode
 *
 * Purpose: encode arbitrary binary data into a "byte stream containing no 0x00", so that a single
 * 0x00 can serve as the frame delimiter. When the serial link drops bytes or the host attaches
 * mid-stream, it can re-align simply by waiting for the next 0x00 -- no complex sync word or length
 * check needed.
 *
 * The overhead is fixed and tiny: at most 1 extra byte per 254 bytes.
 *
 * Reference: Cheshire & Baker, "Consistent Overhead Byte Stuffing", 1999.
 */

#ifndef COBS_H_
#define COBS_H_

#include <stddef.h>
#include <stdint.h>

/** Worst-case output buffer size needed to encode len bytes (excluding the trailing delimiter) */
#define COBS_ENCODED_MAX(len)   ((len) + ((len) / 254) + 1)

/**
 * COBS encode. The output does not include the trailing 0x00 delimiter; the caller appends it.
 *
 * @param in      data to encode
 * @param in_len  length
 * @param out     output buffer, at least COBS_ENCODED_MAX(in_len) bytes
 * @return the number of bytes written to out
 */
size_t cobs_encode(const uint8_t *in, size_t in_len, uint8_t *out);

/**
 * COBS decode. The input should be the content between two 0x00 delimiters (excluding the delimiters themselves).
 *
 * @param in      encoded data
 * @param in_len  length
 * @param out     output buffer, at least in_len bytes
 * @param out_sz  output buffer size
 * @return the number of bytes decoded; returns 0 if the input is invalid or the buffer is too small
 */
size_t cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_sz);

#endif /* COBS_H_ */
