/* COBS encode/decode -- see the notes in cobs.h for the implementation */

#include "cobs.h"

size_t cobs_encode(const uint8_t *in, size_t in_len, uint8_t *out)
{
	size_t read_i = 0;
	size_t write_i = 1;   /* out[0] reserved as a placeholder for the first code */
	size_t code_i = 0;
	uint8_t code = 1;

	while (read_i < in_len) {
		if (in[read_i] == 0) {
			/* Hit a zero byte: write the current run length back into the code slot, start a new run */
			out[code_i] = code;
			code = 1;
			code_i = write_i++;
			read_i++;
			continue;
		}

		out[write_i++] = in[read_i++];
		code++;

		/* A run is at most 254 bytes long; force a break when it is full */
		if (code == 0xFF) {
			out[code_i] = code;
			code = 1;
			code_i = write_i++;
		}
	}

	out[code_i] = code;

	return write_i;
}

size_t cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_sz)
{
	size_t read_i = 0;
	size_t write_i = 0;

	while (read_i < in_len) {
		const uint8_t code = in[read_i];

		if (code == 0) {
			return 0;   /* 0 should never appear in the encoded stream */
		}

		read_i++;

		/* Copy out code-1 non-zero bytes verbatim */
		for (uint8_t i = 1; i < code; i++) {
			if (read_i >= in_len || write_i >= out_sz) {
				return 0;   /* data was truncated, or the output buffer is too small */
			}
			out[write_i++] = in[read_i++];
		}

		/* If the run is not full at 254 and more data follows, the original had a zero byte here */
		if (code != 0xFF && read_i < in_len) {
			if (write_i >= out_sz) {
				return 0;
			}
			out[write_i++] = 0;
		}
	}

	return write_i;
}
