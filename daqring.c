// SPDX-License-Identifier: GPL-2.0
/*
 * daqring - a simulated data-acquisition card driver.
 *
 * Models the kernel side of an FPGA DAQ add-on card without needing the
 * hardware: an hrtimer stands in for the card's sample-ready interrupt,
 * and a vmalloc'd ring buffer stands in for the DMA target. Data reaches
 * user space two ways:
 *
 *   1. read()/poll() on /dev/daqring - blocking, copy-based, simple.
 *   2. mmap() of the ring - zero-copy. Page 0 is a header page whose
 *      `head` field the producer publishes with release semantics, the
 *      way a real card writes back its descriptor pointer; consumers
 *      load it with acquire semantics and index the slots directly.
 *
 * Control is via ioctls (start/stop/rate/stats) and sysfs attributes
 * under /sys/class/misc/daqring/.
 *
 * Locking: `lock` (spinlock, IRQ-safe) protects head/tail/counters and
 * is the only lock the timer callback takes. `cfg_lock` (mutex)
 * serialises configuration paths (rate changes, start/stop, reset).
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/version.h>
#include <linux/math64.h>

#include "daqring.h"

/* Compatibility down to the 4.9-era kernels on old Raspberry Pi OS. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#define sysfs_emit(buf, fmt, ...) scnprintf(buf, PAGE_SIZE, fmt, ##__VA_ARGS__)
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
typedef unsigned int __poll_t;
#define EPOLLIN		POLLIN
#define EPOLLRDNORM	POLLRDNORM
#endif

#define DAQRING_NAME		"daqring"
#define DAQRING_MAX_BURST	256	/* max samples per read() call */
#define DAQRING_DEF_RATE_HZ	1000

static unsigned int ring_pages = 16;
module_param(ring_pages, uint, 0444);
MODULE_PARM_DESC(ring_pages, "Pages of sample storage in the ring (default 16)");

struct daqring_dev {
	/* Shared-memory window: header page + sample slots. */
	void *shm;
	struct daqring_shm_hdr *hdr;
	struct daqring_sample *slots;
	u32 capacity;
	size_t shm_size;

	/* Simulated sample-ready interrupt. */
	struct hrtimer timer;
	ktime_t period;
	u32 rate_hz;
	bool running;
	u32 noise;		/* LCG state for the simulated ADC */

	/* Producer/consumer state, guarded by `lock`. */
	u64 head;
	u64 tail;
	u64 consumed;
	u64 overruns;
	spinlock_t lock;

	struct mutex cfg_lock;
	wait_queue_head_t waitq;
};

static struct daqring_dev ddev;

/*
 * Ring slot for a given sample count. div_u64_rem instead of plain
 * `%`: 32-bit ARM kernels provide no __aeabi_uldivmod for u64 modulo.
 */
static u32 daqring_slot(const struct daqring_dev *dev, u64 seq)
{
	u32 rem;

	div_u64_rem(seq, dev->capacity, &rem);
	return rem;
}

/*
 * Publish the new head to the mmap header; the sample slot must be
 * visible first. On 64-bit this is one release store. A 32-bit CPU
 * cannot store 8 bytes atomically (smp_store_release rejects it at
 * compile time), so publish high word before low: a torn read is then
 * only possible at the 2^32 wrap and only ever jumps forward, which
 * consumers already handle as an overrun resync. Little-endian layout
 * assumed (true of ARM and x86).
 */
static void daqring_publish_head(struct daqring_dev *dev)
{
#ifdef CONFIG_64BIT
	smp_store_release(&dev->hdr->head, dev->head);
#else
	smp_wmb();
	WRITE_ONCE(((u32 *)&dev->hdr->head)[1], upper_32_bits(dev->head));
	WRITE_ONCE(((u32 *)&dev->hdr->head)[0], lower_32_bits(dev->head));
#endif
}

/*
 * Timer callback - runs in hardirq context, exactly like the ISR of the
 * real card would. Writes one sample slot, then publishes the new head.
 */
static enum hrtimer_restart daqring_tick(struct hrtimer *t)
{
	struct daqring_dev *dev = container_of(t, struct daqring_dev, timer);
	struct daqring_sample *slot;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);

	slot = &dev->slots[daqring_slot(dev, dev->head)];
	slot->seq = dev->head;
	slot->timestamp_ns = ktime_get_ns();
	slot->channel = (u32)(dev->head & 0x3);
	/* 12-bit "ADC": sawtooth carrier plus LCG noise in the low bits. */
	dev->noise = dev->noise * 1664525u + 1013904223u;
	slot->value = ((u32)(dev->head & 0x3ff) + (dev->noise >> 28)) & 0xfff;

	dev->head++;

	/* The producer never stalls; a slow read() consumer gets lapped. */
	if (dev->head - dev->tail > dev->capacity) {
		dev->tail = dev->head - dev->capacity;
		dev->overruns++;
		dev->hdr->overruns = dev->overruns;
	}

	daqring_publish_head(dev);

	spin_unlock_irqrestore(&dev->lock, flags);

	wake_up_interruptible(&dev->waitq);

	hrtimer_forward_now(t, dev->period);
	return HRTIMER_RESTART;
}

static bool daqring_data_ready(struct daqring_dev *dev)
{
	return READ_ONCE(dev->head) != READ_ONCE(dev->tail);
}

static ssize_t daqring_read(struct file *file, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct daqring_dev *dev = &ddev;
	struct daqring_sample *tmp;
	unsigned long flags;
	size_t nsamp, i, n;
	ssize_t ret;
	u64 avail;

	nsamp = count / sizeof(*tmp);
	if (!nsamp)
		return -EINVAL;
	nsamp = min_t(size_t, nsamp, DAQRING_MAX_BURST);

	/*
	 * Bounce buffer: samples are copied out of the ring under the
	 * spinlock (bounded, at most DAQRING_MAX_BURST), but the
	 * copy_to_user - which may fault and sleep - happens outside it.
	 */
	tmp = kmalloc_array(nsamp, sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	spin_lock_irqsave(&dev->lock, flags);
	while (dev->head == dev->tail) {
		spin_unlock_irqrestore(&dev->lock, flags);
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}
		if (wait_event_interruptible(dev->waitq,
					     daqring_data_ready(dev))) {
			ret = -ERESTARTSYS;
			goto out;
		}
		spin_lock_irqsave(&dev->lock, flags);
	}

	avail = dev->head - dev->tail;
	n = min_t(u64, avail, nsamp);
	for (i = 0; i < n; i++)
		tmp[i] = dev->slots[daqring_slot(dev, dev->tail + i)];
	dev->tail += n;
	dev->consumed += n;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (copy_to_user(ubuf, tmp, n * sizeof(*tmp)))
		ret = -EFAULT;
	else
		ret = n * sizeof(*tmp);
out:
	kfree(tmp);
	return ret;
}

static __poll_t daqring_poll(struct file *file, poll_table *wait)
{
	struct daqring_dev *dev = &ddev;

	poll_wait(file, &dev->waitq, wait);
	if (daqring_data_ready(dev))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

/* Caller must hold cfg_lock. */
static void __daqring_stop(struct daqring_dev *dev)
{
	if (!dev->running)
		return;
	hrtimer_cancel(&dev->timer);
	dev->running = false;
	dev->hdr->running = 0;
}

/* Caller must hold cfg_lock. */
static void __daqring_start(struct daqring_dev *dev)
{
	if (dev->running)
		return;
	dev->running = true;
	dev->hdr->running = 1;
	hrtimer_start(&dev->timer, dev->period, HRTIMER_MODE_REL);
}

static int daqring_set_rate(struct daqring_dev *dev, u32 hz)
{
	if (hz < 1 || hz > DAQRING_MAX_RATE_HZ)
		return -EINVAL;

	mutex_lock(&dev->cfg_lock);
	dev->rate_hz = hz;
	dev->period = ns_to_ktime(NSEC_PER_SEC / hz);
	if (dev->running) {
		/* Re-arm so the new period takes effect immediately. */
		hrtimer_cancel(&dev->timer);
		hrtimer_start(&dev->timer, dev->period, HRTIMER_MODE_REL);
	}
	mutex_unlock(&dev->cfg_lock);
	return 0;
}

static long daqring_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct daqring_dev *dev = &ddev;
	unsigned long flags;

	switch (cmd) {
	case DAQRING_IOC_START:
		mutex_lock(&dev->cfg_lock);
		__daqring_start(dev);
		mutex_unlock(&dev->cfg_lock);
		return 0;

	case DAQRING_IOC_STOP:
		mutex_lock(&dev->cfg_lock);
		__daqring_stop(dev);
		mutex_unlock(&dev->cfg_lock);
		return 0;

	case DAQRING_IOC_SET_RATE: {
		u32 hz;

		if (get_user(hz, (u32 __user *)arg))
			return -EFAULT;
		return daqring_set_rate(dev, hz);
	}

	case DAQRING_IOC_GET_STATS: {
		struct daqring_stats st;

		spin_lock_irqsave(&dev->lock, flags);
		st.produced = dev->head;
		st.consumed = dev->consumed;
		st.overruns = dev->overruns;
		st.sample_rate_hz = dev->rate_hz;
		st.running = dev->running;
		spin_unlock_irqrestore(&dev->lock, flags);

		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}

	case DAQRING_IOC_RESET: {
		int ret = 0;

		mutex_lock(&dev->cfg_lock);
		if (dev->running) {
			ret = -EBUSY;
		} else {
			spin_lock_irqsave(&dev->lock, flags);
			dev->head = 0;
			dev->tail = 0;
			dev->consumed = 0;
			dev->overruns = 0;
			dev->hdr->head = 0;
			dev->hdr->overruns = 0;
			spin_unlock_irqrestore(&dev->lock, flags);
		}
		mutex_unlock(&dev->cfg_lock);
		return ret;
	}

	default:
		return -ENOTTY;
	}
}

/*
 * Zero-copy path: map the header page plus the sample slots, read-only.
 * VM_MAYWRITE is cleared so a later mprotect(PROT_WRITE) on a shared
 * mapping cannot give user space a writable view of the ring.
 */
static int daqring_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct daqring_dev *dev = &ddev;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff)
		return -EINVAL;
	if (size > dev->shm_size)
		return -EINVAL;
	if (vma->vm_flags & VM_WRITE)
		return -EPERM;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_clear(vma, VM_MAYWRITE);
#else
	vma->vm_flags &= ~VM_MAYWRITE;
#endif

	return remap_vmalloc_range(vma, dev->shm, 0);
}

static int daqring_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}

static const struct file_operations daqring_fops = {
	.owner		= THIS_MODULE,
	.open		= daqring_open,
	.read		= daqring_read,
	.poll		= daqring_poll,
	.unlocked_ioctl	= daqring_ioctl,
	.mmap		= daqring_mmap,
};

/* ---- sysfs: /sys/class/misc/daqring/{sample_rate_hz,produced,...} ---- */

static ssize_t sample_rate_hz_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", ddev.rate_hz);
}

static ssize_t sample_rate_hz_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	u32 hz;
	int ret;

	ret = kstrtou32(buf, 0, &hz);
	if (ret)
		return ret;
	ret = daqring_set_rate(&ddev, hz);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(sample_rate_hz);

static ssize_t produced_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", READ_ONCE(ddev.head));
}
static DEVICE_ATTR_RO(produced);

static ssize_t overruns_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", READ_ONCE(ddev.overruns));
}
static DEVICE_ATTR_RO(overruns);

static ssize_t running_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", ddev.running ? 1 : 0);
}
static DEVICE_ATTR_RO(running);

static struct attribute *daqring_attrs[] = {
	&dev_attr_sample_rate_hz.attr,
	&dev_attr_produced.attr,
	&dev_attr_overruns.attr,
	&dev_attr_running.attr,
	NULL,
};
ATTRIBUTE_GROUPS(daqring);

static struct miscdevice daqring_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= DAQRING_NAME,
	.fops	= &daqring_fops,
	.groups	= daqring_groups,
};

static int __init daqring_init(void)
{
	struct daqring_dev *dev = &ddev;
	int ret;

	if (ring_pages < 1 || ring_pages > 1024)
		return -EINVAL;

	dev->shm_size = (size_t)(ring_pages + 1) * PAGE_SIZE;
	dev->shm = vmalloc_user(dev->shm_size);
	if (!dev->shm)
		return -ENOMEM;

	dev->hdr = dev->shm;
	dev->slots = dev->shm + PAGE_SIZE;
	dev->capacity = (u32)(ring_pages * PAGE_SIZE /
			      sizeof(struct daqring_sample));

	dev->hdr->capacity = dev->capacity;
	dev->hdr->sample_size = sizeof(struct daqring_sample);
	dev->hdr->shm_bytes = (u32)dev->shm_size;

	spin_lock_init(&dev->lock);
	mutex_init(&dev->cfg_lock);
	init_waitqueue_head(&dev->waitq);

	dev->rate_hz = DAQRING_DEF_RATE_HZ;
	dev->period = ns_to_ktime(NSEC_PER_SEC / dev->rate_hz);
	dev->noise = 0x2545f491u;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&dev->timer, daqring_tick, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
#else
	hrtimer_init(&dev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev->timer.function = daqring_tick;
#endif

	ret = misc_register(&daqring_miscdev);
	if (ret) {
		vfree(dev->shm);
		return ret;
	}

	pr_info("daqring: ready, %u slots (%u pages), default %u Hz\n",
		dev->capacity, ring_pages, dev->rate_hz);
	return 0;
}

static void __exit daqring_exit(void)
{
	struct daqring_dev *dev = &ddev;

	mutex_lock(&dev->cfg_lock);
	__daqring_stop(dev);
	mutex_unlock(&dev->cfg_lock);

	misc_deregister(&daqring_miscdev);
	vfree(dev->shm);
	pr_info("daqring: unloaded, produced=%llu overruns=%llu\n",
		dev->head, dev->overruns);
}

module_init(daqring_init);
module_exit(daqring_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joseph Ambrose Pagaran");
MODULE_DESCRIPTION("Simulated DAQ card: hrtimer 'IRQ' feeding a mmap-able ring buffer");
MODULE_VERSION("1.0");
