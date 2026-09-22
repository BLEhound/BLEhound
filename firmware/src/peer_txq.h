/*
 * Inter-board transmit queue — a single-producer single-consumer ring from the radio receive interrupt (producer) to the system work-queue thread (consumer).
 *
 * Why it exists: the inter-board link send (peer_link_send → Zephyr spi_write) is a **synchronous API** that internally
 * k_sem_take-waits for the transfer to complete, which Zephyr forbids calling from an interrupt. tri_coord captures the CONNECT_IND inside the radio ISR,
 * so the ISR only stuffs the encoded message into this queue and submits k_work; the actual SPI transaction is done by the thread.
 *
 * Pure logic, no kernel dependency, testable on host.
 * Concurrency model: exactly one producer (ISR) and one consumer (thread); head is written only by the consumer, tail only by the producer,
 * and byte-wide writes are atomic on single-core Cortex-M, so no lock is needed. When full, the **new** message is dropped and counted (never blocks in the ISR).
 */

#ifndef PEER_TXQ_H_
#define PEER_TXQ_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "peer_msg.h"   /* PEER_MSG_MAX */

/** Queue capacity (messages that can be queued at once): one hit produces at most 2 messages (hit + handoff), so 4 slots of headroom is plenty. */
#define PEER_TXQ_DEPTH 4
/** Ring slot count = capacity + 1: use "keep one empty slot" to distinguish empty/full, so no shared counter is needed. */
#define PEER_TXQ_SLOTS (PEER_TXQ_DEPTH + 1)

struct peer_txq {
	uint8_t buf[PEER_TXQ_SLOTS][PEER_MSG_MAX];
	uint8_t len[PEER_TXQ_SLOTS];
	volatile uint8_t head;    /* position the consumer reads from */
	volatile uint8_t tail;    /* position the producer writes to */
	uint32_t dropped;         /* messages dropped because the queue was full (modified only by the producer) */
};

void peer_txq_init(struct peer_txq *q);

/** Producer: enqueue one message (1..PEER_MSG_MAX bytes). Returns false when full or the length is invalid (dropped++ when full). */
bool peer_txq_push(struct peer_txq *q, const uint8_t *msg, size_t len);

/** Consumer: dequeue the oldest message into out (capacity cap). Returns 0 when empty or cap is too small (does not dequeue when too small). */
size_t peer_txq_pop(struct peer_txq *q, uint8_t *out, size_t cap);

bool peer_txq_empty(const struct peer_txq *q);

uint32_t peer_txq_dropped(const struct peer_txq *q);

#endif /* PEER_TXQ_H_ */
