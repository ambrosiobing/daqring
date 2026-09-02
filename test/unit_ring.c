// SPDX-License-Identifier: GPL-2.0
/*
 * unit_ring - unit tests for the ring arithmetic, the user-space ABI
 * layout, and the head-publication protocol under real concurrency.
 *
 * No framework: a CHECK() macro, TAP-style output, non-zero exit on any
 * failure. Runs in seconds on any host; CI runs it on x86-64 and ARM64.
 *
 * Build:  make unit
 * Run:    ./test/unit_ring
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daqring.h"

/* Exercise the seqlock path even on a 64-bit host: that is the code
 * a 32-bit process runs, and it is the interesting one. */
#define DAQRING_FORCE_SEQ_HEAD 1
#include "daqring_ring.h"

static int checks, failures;

#define CHECK(cond, ...) do {						\
	checks++;							\
	if (cond) {							\
		printf("ok %d - ", checks); printf(__VA_ARGS__); putchar('\n'); \
	} else {							\
		failures++;						\
		printf("not ok %d - ", checks); printf(__VA_ARGS__);	\
		printf("  [%s:%d]\n", __FILE__, __LINE__);		\
	}								\
} while (0)

/* ---- 1. ABI layout: user space and kernel must agree byte for byte ---- */

static void test_abi_layout(void)
{
	CHECK(sizeof(struct daqring_sample) == 24,
	      "sample is 24 bytes (got %zu)", sizeof(struct daqring_sample));
	CHECK(offsetof(struct daqring_sample, seq) == 0, "sample.seq at 0");
	CHECK(offsetof(struct daqring_sample, timestamp_ns) == 8, "sample.timestamp_ns at 8");
	CHECK(offsetof(struct daqring_sample, channel) == 16, "sample.channel at 16");
	CHECK(offsetof(struct daqring_sample, value) == 20, "sample.value at 20");

	CHECK(offsetof(struct daqring_shm_hdr, head) == 0, "hdr.head at 0");
	CHECK(offsetof(struct daqring_shm_hdr, head) % 8 == 0,
	      "hdr.head is 8-byte aligned (needed for the native load)");
	CHECK(offsetof(struct daqring_shm_hdr, head_seq) == 32, "hdr.head_seq at 32");
	CHECK(sizeof(struct daqring_shm_hdr) <= 4096,
	      "header fits in one page (got %zu)", sizeof(struct daqring_shm_hdr));

	CHECK(DAQRING_MAX_RATE_HZ == 100000, "max rate is 100 kHz");
}

/* ---- 2. Ring arithmetic ---- */

static void test_ring_slot(void)
{
	const uint32_t cap = 2730;	/* 16 pages of 24-byte samples */

	CHECK(daqring_ring_slot(0, cap) == 0, "slot(0) = 0");
	CHECK(daqring_ring_slot(cap - 1, cap) == cap - 1, "slot(cap-1) = cap-1");
	CHECK(daqring_ring_slot(cap, cap) == 0, "slot(cap) wraps to 0");
	CHECK(daqring_ring_slot((1ull << 32) + 5, cap) == ((1ull << 32) + 5) % cap,
	      "slot is computed in 64 bits, not truncated to 32");
}

static void test_ring_resync(void)
{
	const uint32_t cap = 2730;
	uint64_t tail, lost;

	tail = 100;
	lost = daqring_ring_resync(100 + cap, &tail, cap);
	CHECK(lost == 0 && tail == 100,
	      "exactly `capacity` ahead is not a lap");

	tail = 100;
	lost = daqring_ring_resync(100 + cap + 1, &tail, cap);
	CHECK(lost == 1 && tail == 101,
	      "one past capacity: 1 lost, tail advanced to oldest survivor");

	tail = 0;
	lost = daqring_ring_resync(10 * (uint64_t)cap, &tail, cap);
	CHECK(lost == 9 * (uint64_t)cap && tail == 9 * (uint64_t)cap,
	      "lapped nine times: loss counted, tail at head-capacity");

	tail = 5000;
	lost = daqring_ring_resync(5000, &tail, cap);
	CHECK(lost == 0 && tail == 5000, "caught up: nothing lost");
}

/* ---- 3. Head publication under concurrency, across the 2^32 wrap ---- */

struct race {
	struct daqring_shm_hdr hdr;
	atomic_uint_fast64_t written;	/* last value the producer stored */
	atomic_int stop;
	uint64_t start;
	uint64_t iterations;
};

static void *producer(void *arg)
{
	struct race *r = arg;
	uint64_t v;

	for (v = r->start; v < r->start + r->iterations; v++) {
		daqring_publish_head(&r->hdr, v);
		atomic_store_explicit(&r->written, v, memory_order_release);
	}
	atomic_store(&r->stop, 1);
	return NULL;
}

struct reader_result {
	uint64_t reads;
	uint64_t backwards;	/* value smaller than the previous read */
	uint64_t phantom;	/* value the producer never wrote */
	uint64_t max_seen;
};

static void *reader(void *arg)
{
	struct race *r = arg;
	struct reader_result *res = calloc(1, sizeof(*res));
	uint64_t prev = 0;

	while (!atomic_load(&r->stop)) {
		uint64_t h = daqring_load_head(&r->hdr);
		uint64_t w = atomic_load_explicit(&r->written, memory_order_acquire);

		res->reads++;
		if (h < prev)
			res->backwards++;
		/*
		 * Anything above what the producer has *finished* writing is
		 * suspicious, but the producer may be one step ahead of its
		 * own bookkeeping; allow exactly that.
		 */
		if (h > w + 1)
			res->phantom++;
		if (h > res->max_seen)
			res->max_seen = h;
		prev = h;
	}
	return res;
}

static void test_head_protocol_across_wrap(void)
{
	struct race r;
	pthread_t p, q;
	struct reader_result *res;

	memset(&r, 0, sizeof(r));
	/* Start just below the 32-bit boundary so the low word wraps
	 * within the first few hundred iterations, then keep wrapping. */
	r.start = (1ull << 32) - 500;
	r.iterations = 3 * (1ull << 10);	/* several thousand updates */
	daqring_publish_head(&r.hdr, r.start - 1);
	atomic_store(&r.written, r.start - 1);

	pthread_create(&q, NULL, reader, &r);
	pthread_create(&p, NULL, producer, &r);
	pthread_join(p, NULL);
	pthread_join(q, (void **)&res);

	CHECK(res->reads > 0, "reader observed %" PRIu64 " loads", res->reads);
	CHECK(res->backwards == 0,
	      "head never went backwards across the 2^32 wrap (%" PRIu64 " violations)",
	      res->backwards);
	CHECK(res->phantom == 0,
	      "reader never saw a value the producer had not written (%" PRIu64 " phantoms)",
	      res->phantom);
	CHECK(res->max_seen == r.start + r.iterations - 1,
	      "final head observed exactly (%" PRIu64 ")", res->max_seen);
	free(res);
}

/* The native 64-bit path, where available, must agree with the seqlock one. */
static void test_head_native_agrees(void)
{
	struct daqring_shm_hdr h;
	uint64_t v = 0x0123456789abcdefull;

	memset(&h, 0, sizeof(h));
	daqring_publish_head(&h, v);
	CHECK(daqring_load_head_seq(&h) == v, "seqlock load returns the published value");
	CHECK(h.head == v, "raw field holds the published value");
	CHECK((h.head_seq & 1) == 0, "sequence is even after a complete publish");
}

int main(void)
{
	test_abi_layout();
	test_ring_slot();
	test_ring_resync();
	test_head_native_agrees();
	test_head_protocol_across_wrap();

	printf("1..%d\n", checks);
	printf("# %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
