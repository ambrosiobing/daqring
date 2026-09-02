/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * daqring_ring.h - the ring's arithmetic and its head-publication
 * protocol, as pure inline functions shared by the user-space tools and
 * their unit tests.
 *
 * The kernel driver implements the same protocol with kernel primitives
 * (WRITE_ONCE/smp_wmb); this header is its user-space mirror and the
 * place where that protocol is tested under real concurrency.
 *
 * The protocol
 * ------------
 * `head` is a 64-bit counter that only ever increases. A 64-bit reader
 * acquire-loads it in one instruction and is done. A 32-bit reader
 * cannot: the two halves are loaded separately, and around the 2^32
 * wrap it can observe the new high word with the old low word - a
 * value the producer never wrote, followed by a *smaller* value on the
 * next read. So the producer brackets every update with a sequence
 * counter (odd = update in progress, even = stable), and a 32-bit
 * reader retries until it sees the same even sequence on both sides
 * of its two loads. Classic seqlock; costs the producer two extra
 * 32-bit stores per sample.
 */
#ifndef DAQRING_RING_H
#define DAQRING_RING_H

#include <stdint.h>
#include <stdbool.h>

#include "daqring.h"

/* Slot index for a given sample count. */
static inline uint32_t daqring_ring_slot(uint64_t seq, uint32_t capacity)
{
	return (uint32_t)(seq % capacity);
}

/*
 * A consumer more than `capacity` behind has been lapped: the producer
 * has overwritten what it was about to read. Advance *tail to the
 * oldest surviving sample and return how many were lost.
 */
static inline uint64_t daqring_ring_resync(uint64_t head, uint64_t *tail,
					   uint32_t capacity)
{
	uint64_t behind = head - *tail;
	uint64_t lost;

	if (behind <= capacity)
		return 0;
	lost = behind - capacity;
	*tail = head - capacity;
	return lost;
}

/*
 * Producer side of the protocol, for user-space producers and tests.
 * The kernel driver does exactly this with WRITE_ONCE and smp_wmb.
 */
static inline void daqring_publish_head(volatile struct daqring_shm_hdr *h,
					uint64_t value)
{
	volatile uint32_t *w = (volatile uint32_t *)&h->head;
	uint32_t s = __atomic_load_n(&h->head_seq, __ATOMIC_RELAXED) + 1;

	__atomic_store_n(&h->head_seq, s, __ATOMIC_RELAXED);	/* odd */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&w[1], (uint32_t)(value >> 32), __ATOMIC_RELAXED);
	__atomic_store_n(&w[0], (uint32_t)value, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&h->head_seq, s + 1, __ATOMIC_RELAXED);	/* even */
}

/*
 * Seqlock read of the 64-bit head from two 32-bit loads. Safe on a
 * PROT_READ mapping - it only ever loads - and correct across the wrap.
 */
static inline uint64_t daqring_load_head_seq(const volatile struct daqring_shm_hdr *h)
{
	const volatile uint32_t *w = (const volatile uint32_t *)&h->head;
	uint32_t s1, s2, hi, lo;

	do {
		s1 = __atomic_load_n(&h->head_seq, __ATOMIC_RELAXED);
		if (s1 & 1)
			continue;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		hi = __atomic_load_n(&w[1], __ATOMIC_RELAXED);
		lo = __atomic_load_n(&w[0], __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		s2 = __atomic_load_n(&h->head_seq, __ATOMIC_RELAXED);
	} while (s1 != s2);

	return ((uint64_t)hi << 32) | lo;
}

/*
 * Acquire-load the head, choosing the native 8-byte load when the
 * platform has one. DAQRING_FORCE_SEQ_HEAD makes the unit tests
 * exercise the seqlock path on 64-bit hosts too.
 */
static inline uint64_t daqring_load_head(const volatile struct daqring_shm_hdr *h)
{
#if (defined(__LP64__) || defined(_LP64)) && !defined(DAQRING_FORCE_SEQ_HEAD)
	return __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
#else
	return daqring_load_head_seq(h);
#endif
}

#endif /* DAQRING_RING_H */
