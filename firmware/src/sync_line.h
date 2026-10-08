/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SYNC line HAL -- the open-drain edge shared by the three boards that establishes a common cross-board
 * time base (design 5.2 / 3.3).
 *
 * Whoever catches a CONNECT_IND (or periodically) calls sync_line_emit() to pull an edge; all three boards
 * capture that edge with a GPIO interrupt at a tick of **their own TIMER time base** (the same base as
 * radio_now_us), so one physical event leaves one tick in each of the three clocks -> the host derives the
 * cross-board clock offsets and aligns the three streams (see blehound_tri_aggregator).
 *
 * Implementation note (relation to the design):
 *   Design 3.3 envisions **hardware capture** "GPIOTE edge -> DPPI -> TIMER.CAPTURE" (zero software latency).
 *   This file uses a portable implementation with **Zephyr GPIO interrupt + radio_now_us()**: one code base
 *   for nRF52/nRF54L, and it can be regression-tested on a DK with two GPIOs jumpered. The price is interrupt
 *   latency on the captured tick (microsecond-level jitter) -- more than enough for "periodic sync edges to
 *   align the three streams" (3.3 says once per second is enough).
 *   Bring-up / optimisation: for per-event anchor precision the capture could be switched to the DPPI
 *   hardware capture used in radio_nrf54l.c (PUBLISH/SUBSCRIBE, TIMER10 CC[2] reserved).
 */

#ifndef SYNC_LINE_H_
#define SYNC_LINE_H_

#include <stddef.h>
#include <stdint.h>

/** Called when a SYNC edge is captured, with the tick of that edge in this board's TIMER time base (us).
 *  Interrupt context, keep it very short. */
typedef void (*sync_capture_cb_t)(uint32_t local_tick);

/**
 * Initialise the SYNC line: configure the input pin (falling-edge interrupt) and the output pin (open drain).
 * Returns 0 but stays disabled (last_capture always 0) when the tri-sync-in/out aliases are not defined
 * (e.g. single board).
 * @return 0 on success or when disabled; negative errno otherwise
 */
int sync_line_init(sync_capture_cb_t on_capture);

/** Pull one SYNC edge from this board (open drain: drive low for a few us, then release). The line is shared by
 *  all three boards, so this board captures it as well. Returns this board's tick at the moment of driving low
 *  (us; 0 when disabled) -- the discovering board uses it as "this edge" without relying on its own capture
 *  interrupt (which may be delayed when called from the radio interrupt). */
uint32_t sync_line_emit(void);

/** Maximum number of recently captured edge ticks kept (ring, newest overwrites oldest). */
#define SYNC_LINE_RECENT_MAX 4

/** Copy out the most recently captured edge ticks (newest -> oldest); returns the count (<= cap). Any context. */
size_t sync_line_recent_captures(uint32_t *out, size_t cap);

/** Log a diagnostic set: pin PIN_CNF/level read-back, GPIOTE channel configuration and event flags
 *  (to locate "pulsed but never captured"). */
void sync_line_log_debug(void);

/** Total edges captured by the ISR (including our own pulses when hardware self-capture works). Diagnostics. */
uint32_t sync_line_capture_count(void);

/** Tick of the most recently captured edge (this board's TIMER time base, us; 0 = none yet). */
uint32_t sync_line_last_capture(void);

#endif /* SYNC_LINE_H_ */
