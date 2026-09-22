/*
 * Board role identification — all three boards run identical firmware and read 2 strap GPIOs at boot to self-identify their role.
 *
 * On each of the three custom boards one strap group is hard-wired (design §4.5: 2 GPIOs coded via tie-to-GND / pull-up),
 * deciding whether this board is A/B/C and which of the 37/38/39 advertising channels it guards. The firmware binary is identical across all three boards;
 * the role is determined purely at runtime by the straps — no separate firmware build per board.
 *
 * strap encoding (S1,S0):
 *   00 → board_id 0, guards channel 37 (node A)
 *   10 → board_id 1, guards channel 38 (node B)
 *   01 → board_id 2, guards channel 39 (node C)
 *   11 → reserved, falls back to {0, 37} and warns
 *
 * A strap tied to GND reads as 0; floating with the internal pull-up it reads as 1. On the DK (which lacks this strap group) the DT alias does not exist,
 * so board_role_read() falls back to {0, 37}, keeping the single-board firmware running as usual.
 */

#ifndef BOARD_ROLE_H_
#define BOARD_ROLE_H_

#include <stdint.h>

struct board_role {
	uint8_t board_id;        /**< This board's id 0/1/2 */
	uint8_t guard_channel;   /**< The advertising channel this board guards 37/38/39 */
};

/**
 * Read the straps to decide the role. Falls back to {0, 37} with LOG_WRN when the DT strap alias is undefined or the read fails.
 * Idempotent, may be called multiple times; internally configures the strap pins as input + pull-up.
 */
struct board_role board_role_read(void);

#endif /* BOARD_ROLE_H_ */
