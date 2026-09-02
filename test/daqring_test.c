// SPDX-License-Identifier: GPL-2.0
/*
 * daqring_test - exercises both data paths of the daqring module.
 *
 *   daqring_test read <rate_hz> <seconds>   poll()+read() path
 *   daqring_test mmap <rate_hz> <seconds>   zero-copy mmap path
 *
 * Both modes verify sample sequence continuity (every gap is counted),
 * then print achieved throughput next to the kernel's own counters.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "daqring.h"
#include "daqring_ring.h"	/* daqring_load_head, daqring_ring_* */

#define DEVICE "/dev/daqring"
#define BATCH 256

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void print_stats(int fd, uint64_t got, uint64_t gaps, double secs)
{
	struct daqring_stats st;

	printf("consumed %" PRIu64 " samples in %.2f s (%.0f samples/s), "
	       "%" PRIu64 " sequence gaps\n",
	       got, secs, (double)got / secs, gaps);

	if (ioctl(fd, DAQRING_IOC_GET_STATS, &st) == 0)
		printf("kernel:  produced=%" PRIu64 " consumed=%" PRIu64
		       " overruns=%" PRIu64 " rate=%u Hz\n",
		       (uint64_t)st.produced, (uint64_t)st.consumed,
		       (uint64_t)st.overruns, st.sample_rate_hz);
}

static int run_read(int fd, int seconds)
{
	struct daqring_sample buf[BATCH];
	uint64_t got = 0, gaps = 0, expect = 0;
	int have_expect = 0;
	uint64_t t0 = now_ns();
	uint64_t deadline = t0 + (uint64_t)seconds * 1000000000ull;

	while (now_ns() < deadline) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;
		int i;

		if (poll(&pfd, 1, 100) <= 0)
			continue;

		n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			perror("read");
			return 1;
		}
		n /= (ssize_t)sizeof(buf[0]);
		for (i = 0; i < n; i++) {
			if (have_expect && buf[i].seq != expect)
				gaps++;
			expect = buf[i].seq + 1;
			have_expect = 1;
		}
		got += (uint64_t)n;
	}

	print_stats(fd, got, gaps, (double)(now_ns() - t0) / 1e9);
	return 0;
}

static int run_mmap(int fd, int seconds)
{
	struct daqring_shm_hdr *hdr;
	struct daqring_sample *slots;
	uint32_t shm_bytes, capacity;
	uint64_t tail, got = 0, gaps = 0;
	uint64_t t0, deadline;
	void *map;
	long page = sysconf(_SC_PAGESIZE);

	/* Map the header page first to learn the full window size. */
	map = mmap(NULL, (size_t)page, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap hdr");
		return 1;
	}
	hdr = map;
	shm_bytes = hdr->shm_bytes;
	capacity = hdr->capacity;
	munmap(map, (size_t)page);

	map = mmap(NULL, shm_bytes, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap ring");
		return 1;
	}
	hdr = map;
	slots = (struct daqring_sample *)((char *)map + page);

	printf("mapped %u bytes, %u slots\n", shm_bytes, capacity);
	if (getenv("DAQRING_DEBUG"))
		fprintf(stderr, "[dbg] mapped, entering consume loop\n");

	/* Start consuming from wherever the producer is right now. */
	tail = daqring_load_head(hdr);
	t0 = now_ns();
	deadline = t0 + (uint64_t)seconds * 1000000000ull;

	while (now_ns() < deadline) {
		/* Acquire pairs with the kernel's publish; see daqring_ring.h. */
		uint64_t head = daqring_load_head(hdr);

		if (head <= tail) {
			struct timespec ts = { 0, 200000 }; /* 200 us */

			nanosleep(&ts, NULL);
			continue;
		}
		/* Producer lapped us: resync and count the loss. */
		gaps += daqring_ring_resync(head, &tail, capacity);
		for (; tail != head; tail++) {
			const struct daqring_sample *s =
				&slots[daqring_ring_slot(tail, capacity)];

			if (s->seq != tail)
				gaps++;
			got++;
		}
	}

	print_stats(fd, got, gaps, (double)(now_ns() - t0) / 1e9);
	munmap(map, shm_bytes);
	return 0;
}

int main(int argc, char **argv)
{
	uint32_t rate;
	int fd, seconds, ret;

	if (argc != 4 ||
	    (strcmp(argv[1], "read") && strcmp(argv[1], "mmap"))) {
		fprintf(stderr,
			"usage: %s read|mmap <rate_hz> <seconds>\n", argv[0]);
		return 2;
	}
	rate = (uint32_t)strtoul(argv[2], NULL, 0);
	seconds = atoi(argv[3]);

	/* Line-buffered: a crash must not swallow what we printed. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	fd = open(DEVICE, O_RDONLY);
	if (fd < 0) {
		perror("open " DEVICE);
		return 1;
	}

	if (ioctl(fd, DAQRING_IOC_SET_RATE, &rate) < 0) {
		perror("SET_RATE");
		close(fd);
		return 1;
	}
	if (ioctl(fd, DAQRING_IOC_START) < 0) {
		perror("START");
		close(fd);
		return 1;
	}

	printf("[%s] %u Hz for %d s\n", argv[1], rate, seconds);
	ret = strcmp(argv[1], "read") == 0 ? run_read(fd, seconds)
					   : run_mmap(fd, seconds);

	ioctl(fd, DAQRING_IOC_STOP);
	close(fd);
	return ret;
}
