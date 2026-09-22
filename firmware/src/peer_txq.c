/*
 * Inter-board transmit queue implementation — see peer_txq.h.
 */

#include <string.h>

#include "peer_txq.h"

/* Compiler barrier: on single-core the producer (ISR) and consumer (thread) are naturally serialized and the hardware won't reorder; all we must prevent is
 * the compiler moving "write data" after "publish index" (or hoisting "read data" before "read index"). */
#define COMPILER_BARRIER() __asm__ volatile("" ::: "memory")

/* Ring index: use "keep one empty slot" to detect full — head==tail is empty, (tail+1)%N==head is full. */
static uint8_t next_idx(uint8_t i)
{
	return (uint8_t)((i + 1u) % PEER_TXQ_SLOTS);
}

void peer_txq_init(struct peer_txq *q)
{
	memset(q, 0, sizeof(*q));
}

bool peer_txq_push(struct peer_txq *q, const uint8_t *msg, size_t len)
{
	if (msg == NULL || len == 0u || len > PEER_MSG_MAX) {
		return false;
	}

	const uint8_t tail = q->tail;
	const uint8_t nt = next_idx(tail);

	if (nt == q->head) {
		q->dropped++;
		return false;
	}

	memcpy(q->buf[tail], msg, len);
	q->len[tail] = (uint8_t)len;
	COMPILER_BARRIER();
	q->tail = nt;   /* publish last, so the data is ready by the time the consumer sees tail */
	return true;
}

size_t peer_txq_pop(struct peer_txq *q, uint8_t *out, size_t cap)
{
	const uint8_t head = q->head;

	if (head == q->tail) {
		return 0;
	}
	COMPILER_BARRIER();

	const size_t len = q->len[head];

	if (out == NULL || cap < len) {
		return 0;
	}

	memcpy(out, q->buf[head], len);
	COMPILER_BARRIER();
	q->head = next_idx(head);   /* release the slot only after the data is read, so the producer may overwrite it */
	return len;
}

bool peer_txq_empty(const struct peer_txq *q)
{
	return q->head == q->tail;
}

uint32_t peer_txq_dropped(const struct peer_txq *q)
{
	return q->dropped;
}
