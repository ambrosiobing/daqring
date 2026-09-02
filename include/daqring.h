/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * daqring - user-space ABI for the simulated data-acquisition ring driver.
 *
 * Shared between the kernel module and user-space consumers. Everything
 * here uses fixed-width types and explicit padding so the layout is the
 * same for a 32-bit process on a 64-bit kernel.
 */
#ifndef _UAPI_DAQRING_H
#define _UAPI_DAQRING_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* One acquisition sample, 24 bytes. Modelled on a timestamped ADC word. */
struct daqring_sample {
	__u64 seq;          /* free-running sample counter */
	__u64 timestamp_ns; /* CLOCK_MONOTONIC at production time */
	__u32 channel;      /* simulated input channel, 0..3 */
	__u32 value;        /* simulated 12-bit ADC reading */
};

/*
 * Page 0 of the mmap() window. Plays the role of a DMA write-back /
 * descriptor page on a real card: the producer publishes `head` with
 * release semantics after the sample slot is written; consumers must
 * read it with acquire semantics before touching the slots.
 */
struct daqring_shm_hdr {
	__u64 head;        /* samples produced so far; slot = (head-1) % capacity */
	__u64 overruns;    /* times the producer lapped the read() consumer */
	__u32 capacity;    /* number of sample slots in the ring */
	__u32 sample_size; /* sizeof(struct daqring_sample) */
	__u32 shm_bytes;   /* total mmap window size (this page + slots) */
	__u32 running;     /* 1 while acquisition is running */
	/*
	 * Seqlock around `head` for readers that cannot load 8 bytes
	 * atomically (32-bit user space, on any kernel). Odd while the
	 * producer is mid-update, even when `head` is stable. 64-bit readers
	 * may ignore it and acquire-load `head` directly.
	 */
	__u32 head_seq;
	__u32 reserved;
};

struct daqring_stats {
	__u64 produced;
	__u64 consumed;    /* via the read() path only */
	__u64 overruns;
	__u32 sample_rate_hz;
	__u32 running;
};

#define DAQRING_IOC_MAGIC	'D'

#define DAQRING_IOC_START	_IO(DAQRING_IOC_MAGIC, 0)
#define DAQRING_IOC_STOP	_IO(DAQRING_IOC_MAGIC, 1)
#define DAQRING_IOC_SET_RATE	_IOW(DAQRING_IOC_MAGIC, 2, __u32)
#define DAQRING_IOC_GET_STATS	_IOR(DAQRING_IOC_MAGIC, 3, struct daqring_stats)
#define DAQRING_IOC_RESET	_IO(DAQRING_IOC_MAGIC, 4)

#define DAQRING_MAX_RATE_HZ	100000

#endif /* _UAPI_DAQRING_H */
