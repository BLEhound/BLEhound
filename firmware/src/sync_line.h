/*
 * SYNC line HAL — an open-drain edge shared by the three boards, used to establish a cross-board common time base (design §5.2 / §3.3).
 *
 * Whoever captures a CONNECT_IND (or on a periodic basis) calls sync_line_emit() to pull an edge; all three boards use a GPIO interrupt
 * to capture that edge's tick in **their own TIMER time base** (the same time base as radio_now_us), so the same physical event leaves
 * one tick in each of the three clocks → the host uses this to solve the cross-board clock offset and align the three timestamp streams (see tri_aggregator).
 *
 * Implementation notes (relation to the design):
 *   Design §3.3 envisions **hardware capture** via "GPIOTE edge → DPPI → TIMER.CAPTURE" (zero software latency).
 *   This file uses a portable implementation with **a Zephyr GPIO interrupt + radio_now_us()**: one codebase fits both nRF52/
 *   nRF54L, and can be regression-tested on the DK by jumpering two GPIOs together. The cost is that the capture instant carries interrupt latency (µs-level jitter) —
 *   which is entirely sufficient for "a periodic sync edge aligning the three merged streams" (§3.3 says once per second is enough).
 *   ⚠️ Verify/optimize on hardware: to reach per-event anchor-level precision, the capture can be swapped for the DPPI hardware capture from radio_nrf54l.c's
 *     PUBLISH/SUBSCRIBE mechanism (TIMER10 CC[2] is already reserved).
 */

#ifndef SYNC_LINE_H_
#define SYNC_LINE_H_

#include <stdint.h>

/** Callback when a SYNC edge is captured, giving that edge's tick in this board's TIMER time base (µs). Interrupt context, extremely short. */
typedef void (*sync_capture_cb_t)(uint32_t local_tick);

/**
 * Initialize the SYNC line. Configures the input pin (falling-edge interrupt) and the output pin (open-drain).
 * Returns 0 but stays functionally disabled (last_capture always 0) when the tri-sync-in/out DT alias is undefined (e.g. single board).
 * @return 0 on success or when disabled; negative for errno
 */
int sync_line_init(sync_capture_cb_t on_capture);

/** This board pulls a SYNC edge (open-drain: drive low for a few µs, then release). The line is shared by three boards, so this board captures it too. */
void sync_line_emit(void);

/** Emit a batch of diagnostic logs: pin PIN_CNF / level read-back, GPIOTE channel config and event flags (to pinpoint "pulse sent but nothing captured"). */
void sync_line_log_debug(void);

/** Total number of edges captured by the ISR (including this board's own pulses, if hardware self-capture holds). For diagnostics. */
uint32_t sync_line_capture_count(void);

/** Read the most recently captured edge tick (this board's TIMER time base, µs; 0 = none yet). */
uint32_t sync_line_last_capture(void);

#endif /* SYNC_LINE_H_ */
