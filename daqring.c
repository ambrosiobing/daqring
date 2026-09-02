// SPDX-License-Identifier: GPL-2.0
/*
 * daqring - a simulated data-acquisition card driver.
 *
 * v2: a platform driver probed from the device tree. The "card" is two
 * GPIO pins joined by a jumper wire: an hrtimer pulses the trigger
 * output at the sample rate, the looped-back edge arrives as a real
 * hardware interrupt on the irq input, and the ISR timestamps it,
 * measures trigger-to-ISR latency, and writes a sample into a
 * DMA-style ring buffer. Without the device-tree node (or on a board
 * with no GPIOs) the driver falls back to v1 behaviour: the hrtimer
 * produces samples directly, in simulation mode.
 *
 * Data reaches user space two ways:
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
 * the latency statistics; it is the only lock the timer callback and
 * the ISR take. `cfg_lock` (mutex) serialises configuration paths
 * (rate changes, start/stop, reset).
 *
 * Interrupt split: the hardirq half timestamps the edge, updates the
 * latency histogram and writes the sample slot (the equivalent of
 * draining a card FIFO in the ISR); the threaded half wakes sleeping
 * readers, deferring everything the hardirq does not strictly need.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
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
#include <linux/atomic.h>

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
#define DAQRING_LAT_BUCKETS	8

static unsigned int ring_pages = 16;
module_param(ring_pages, uint, 0444);
MODULE_PARM_DESC(ring_pages, "Pages of sample storage in the ring (default 16, DT ring-pages wins)");

/* Latency histogram bucket upper bounds, in ns. */
static const u64 daqring_lat_edge[DAQRING_LAT_BUCKETS] = {
	5000, 10000, 20000, 50000, 100000, 200000, 500000, ~0ULL,
};

struct daqring_dev {
	/* Shared-memory window: header page + sample slots. */
	void *shm;
	struct daqring_shm_hdr *hdr;
	struct daqring_sample *slots;
	u32 capacity;
	size_t shm_size;

	/* The "card": trigger output looped to an interrupt input. */
	bool hw_mode;
	struct gpio_desc *trigger;
	struct gpio_desc *irq_gpiod;
	int irq;
	atomic64_t toggle_ns;	/* when the last trigger pulse was raised */

	/* Sample clock: pulses the trigger (hw) or produces (sim). */
	struct hrtimer timer;
	ktime_t period;
	u32 rate_hz;
	bool running;
	u32 noise;		/* LCG state for the simulated ADC */

	/* Producer/consumer state and stats, guarded by `lock`. */
	u64 head;
	u64 tail;
	u64 consumed;
	u64 overruns;
	u64 pulses;		/* trigger edges emitted (hw mode) */
	u64 lat_cnt;		/* IRQs whose latency was measured */
	u64 lat_min;
	u64 lat_max;
	u64 lat_sum;
	u32 lat_hist[DAQRING_LAT_BUCKETS];
	spinlock_t lock;

	struct mutex cfg_lock;
	wait_queue_head_t waitq;

	struct platform_device *pdev;
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
 * visible first.
 *
 * The store itself is one release store on 64-bit. A 32-bit CPU cannot
 * store 8 bytes atomically (smp_store_release rejects it at compile
 * time), so there the two halves go out separately, high word first.
 *
 * Either way the update is bracketed by a seqlock counter: odd while
 * in progress, even when stable. That exists for *readers* that cannot
 * load 8 bytes atomically - 32-bit user space on any kernel, including
 * a 64-bit one - which would otherwise observe a torn value around the
 * 2^32 wrap. A unit test (test/unit_ring.c) provokes exactly that wrap
 * and checks the protocol under real concurrency. 64-bit readers may
 * still acquire-load `head` directly and ignore the sequence.
 */
static void daqring_publish_head(struct daqring_dev *dev)
{
	struct daqring_shm_hdr *hdr = dev->hdr;
	u32 seq = READ_ONCE(hdr->head_seq) + 1;

	WRITE_ONCE(hdr->head_seq, seq);		/* odd: update in progress */
	smp_wmb();
#ifdef CONFIG_64BIT
	WRITE_ONCE(hdr->head, dev->head);
#else
	WRITE_ONCE(((u32 *)&hdr->head)[1], upper_32_bits(dev->head));
	WRITE_ONCE(((u32 *)&hdr->head)[0], lower_32_bits(dev->head));
#endif
	smp_wmb();
	WRITE_ONCE(hdr->head_seq, seq + 1);	/* even: stable */
}

/* Write one sample into the ring. Caller holds `lock`. */
static void daqring_produce_locked(struct daqring_dev *dev, u64 timestamp_ns)
{
	struct daqring_sample *slot;

	slot = &dev->slots[daqring_slot(dev, dev->head)];
	slot->seq = dev->head;
	slot->timestamp_ns = timestamp_ns;
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
}

/*
 * Sample clock, hardirq context. In hardware mode it only pulses the
 * trigger line - the sample is produced when the edge comes back as an
 * interrupt. In simulation mode it produces the sample directly (v1
 * behaviour).
 */
static enum hrtimer_restart daqring_tick(struct hrtimer *t)
{
	struct daqring_dev *dev = container_of(t, struct daqring_dev, timer);
	unsigned long flags;

	if (dev->hw_mode) {
		atomic64_set(&dev->toggle_ns, ktime_get_ns());
		gpiod_set_value(dev->trigger, 1);
		gpiod_set_value(dev->trigger, 0);
		spin_lock_irqsave(&dev->lock, flags);
		dev->pulses++;
		spin_unlock_irqrestore(&dev->lock, flags);
	} else {
		spin_lock_irqsave(&dev->lock, flags);
		daqring_produce_locked(dev, ktime_get_ns());
		spin_unlock_irqrestore(&dev->lock, flags);
		wake_up_interruptible(&dev->waitq);
	}

	hrtimer_forward_now(t, dev->period);
	return HRTIMER_RESTART;
}

/*
 * Hardirq half: timestamp the edge, account trigger-to-ISR latency,
 * write the sample - the moral equivalent of reading the card's FIFO
 * in the ISR. Wake-ups are deferred to the threaded half.
 */
static irqreturn_t daqring_irq(int irq, void *data)
{
	struct daqring_dev *dev = data;
	u64 now = ktime_get_ns();
	u64 t = (u64)atomic64_read(&dev->toggle_ns);
	unsigned long flags;
	int i;

	spin_lock_irqsave(&dev->lock, flags);
	if (t && now > t) {
		u64 d = now - t;

		dev->lat_cnt++;
		dev->lat_sum += d;
		if (d < dev->lat_min)
			dev->lat_min = d;
		if (d > dev->lat_max)
			dev->lat_max = d;
		for (i = 0; i < DAQRING_LAT_BUCKETS; i++) {
			if (d <= daqring_lat_edge[i]) {
				dev->lat_hist[i]++;
				break;
			}
		}
	}
	daqring_produce_locked(dev, now);
	spin_unlock_irqrestore(&dev->lock, flags);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t daqring_irq_thread(int irq, void *data)
{
	struct daqring_dev *dev = data;

	wake_up_interruptible(&dev->waitq);
	return IRQ_HANDLED;
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
			dev->pulses = 0;
			dev->lat_cnt = 0;
			dev->lat_sum = 0;
			dev->lat_min = ~0ULL;
			dev->lat_max = 0;
			memset(dev->lat_hist, 0, sizeof(dev->lat_hist));
			dev->hdr->head = 0;
			dev->hdr->overruns = 0;
			dev->hdr->head_seq = 0;
			spin_unlock_irqrestore(&dev->lock, flags);
			atomic64_set(&dev->toggle_ns, 0);
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

/* ---- sysfs: /sys/class/misc/daqring/ ---- */

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

static ssize_t mode_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  ddev.hw_mode ? "hardware" : "simulation");
}
static DEVICE_ATTR_RO(mode);

static ssize_t irq_latency_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	u64 cnt, mn, mx, sum, pulses, avg;
	unsigned long flags;

	spin_lock_irqsave(&ddev.lock, flags);
	cnt = ddev.lat_cnt;
	mn = ddev.lat_min;
	mx = ddev.lat_max;
	sum = ddev.lat_sum;
	pulses = ddev.pulses;
	spin_unlock_irqrestore(&ddev.lock, flags);

	avg = cnt ? div64_u64(sum, cnt) : 0;
	if (!cnt)
		mn = 0;

	return sysfs_emit(buf,
		"pulses=%llu irqs=%llu min_ns=%llu avg_ns=%llu max_ns=%llu\n",
		pulses, cnt, mn, avg, mx);
}
static DEVICE_ATTR_RO(irq_latency);

static ssize_t irq_latency_hist_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	static const char * const label[DAQRING_LAT_BUCKETS] = {
		"<=5us", "<=10us", "<=20us", "<=50us",
		"<=100us", "<=200us", "<=500us", ">500us",
	};
	u32 hist[DAQRING_LAT_BUCKETS];
	unsigned long flags;
	ssize_t len = 0;
	int i;

	spin_lock_irqsave(&ddev.lock, flags);
	memcpy(hist, ddev.lat_hist, sizeof(hist));
	spin_unlock_irqrestore(&ddev.lock, flags);

	for (i = 0; i < DAQRING_LAT_BUCKETS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%-8s %u\n",
				 label[i], hist[i]);
	return len;
}
static DEVICE_ATTR_RO(irq_latency_hist);

static struct attribute *daqring_attrs[] = {
	&dev_attr_sample_rate_hz.attr,
	&dev_attr_produced.attr,
	&dev_attr_overruns.attr,
	&dev_attr_running.attr,
	&dev_attr_mode.attr,
	&dev_attr_irq_latency.attr,
	&dev_attr_irq_latency_hist.attr,
	NULL,
};
ATTRIBUTE_GROUPS(daqring);

static struct miscdevice daqring_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= DAQRING_NAME,
	.fops	= &daqring_fops,
	.groups	= daqring_groups,
};

/* ---- platform driver ---- */

static int daqring_probe(struct platform_device *pdev)
{
	struct device *d = &pdev->dev;
	struct daqring_dev *dev = &ddev;
	u32 pages = ring_pages;
	int ret;

	if (dev->pdev)
		return -EBUSY;	/* single instance */

	device_property_read_u32(d, "ring-pages", &pages);
	if (pages < 1 || pages > 1024)
		return -EINVAL;

	dev->shm_size = (size_t)(pages + 1) * PAGE_SIZE;
	dev->shm = vmalloc_user(dev->shm_size);
	if (!dev->shm)
		return -ENOMEM;

	dev->hdr = dev->shm;
	dev->slots = dev->shm + PAGE_SIZE;
	dev->capacity = (u32)(pages * PAGE_SIZE /
			      sizeof(struct daqring_sample));

	dev->hdr->capacity = dev->capacity;
	dev->hdr->sample_size = sizeof(struct daqring_sample);
	dev->hdr->shm_bytes = (u32)dev->shm_size;

	spin_lock_init(&dev->lock);
	mutex_init(&dev->cfg_lock);
	init_waitqueue_head(&dev->waitq);
	atomic64_set(&dev->toggle_ns, 0);
	dev->lat_min = ~0ULL;

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

	/* The card's two lines; absent on the fallback sim device. */
	dev->trigger = devm_gpiod_get_optional(d, "trigger", GPIOD_OUT_LOW);
	if (IS_ERR(dev->trigger)) {
		ret = PTR_ERR(dev->trigger);
		goto err_free;
	}
	dev->irq_gpiod = devm_gpiod_get_optional(d, "irq", GPIOD_IN);
	if (IS_ERR(dev->irq_gpiod)) {
		ret = PTR_ERR(dev->irq_gpiod);
		goto err_free;
	}

	if (dev->trigger && dev->irq_gpiod) {
		dev->irq = gpiod_to_irq(dev->irq_gpiod);
		if (dev->irq < 0) {
			ret = dev->irq;
			goto err_free;
		}
		ret = devm_request_threaded_irq(d, dev->irq, daqring_irq,
						daqring_irq_thread,
						IRQF_TRIGGER_RISING,
						DAQRING_NAME, dev);
		if (ret)
			goto err_free;
		dev->hw_mode = true;
	} else {
		dev->hw_mode = false;
		dev_warn(d, "no trigger/irq GPIOs, running in simulation mode\n");
	}

	ret = misc_register(&daqring_miscdev);
	if (ret)
		goto err_free;

	dev->pdev = pdev;
	platform_set_drvdata(pdev, dev);

	dev_info(d, "ready: %s mode, %u slots (%u pages), default %u Hz%s\n",
		 dev->hw_mode ? "hardware" : "simulation",
		 dev->capacity, pages, dev->rate_hz,
		 dev->hw_mode ? ", irq armed" : "");
	return 0;

err_free:
	vfree(dev->shm);
	dev->shm = NULL;
	return ret;
}

static void __daqring_teardown(struct platform_device *pdev)
{
	struct daqring_dev *dev = platform_get_drvdata(pdev);

	mutex_lock(&dev->cfg_lock);
	__daqring_stop(dev);
	mutex_unlock(&dev->cfg_lock);

	misc_deregister(&daqring_miscdev);
	vfree(dev->shm);
	dev->shm = NULL;
	dev->pdev = NULL;

	dev_info(&pdev->dev, "unloaded, produced=%llu overruns=%llu\n",
		 dev->head, dev->overruns);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void daqring_remove(struct platform_device *pdev)
{
	__daqring_teardown(pdev);
}
#else
static int daqring_remove(struct platform_device *pdev)
{
	__daqring_teardown(pdev);
	return 0;
}
#endif

static const struct of_device_id daqring_of_match[] = {
	{ .compatible = "jap,daqring" },
	{ }
};
MODULE_DEVICE_TABLE(of, daqring_of_match);

static struct platform_driver daqring_driver = {
	.probe	= daqring_probe,
	.remove	= daqring_remove,
	.driver	= {
		.name		= DAQRING_NAME,
		.of_match_table	= daqring_of_match,
	},
};

/*
 * If no device-tree node describes the card, register a bare platform
 * device so the driver still binds (by name) and runs in simulation
 * mode - keeps the module usable on any machine, DT or not.
 */
static struct platform_device *daqring_sim_pdev;

static int __init daqring_init(void)
{
	struct device_node *np;
	int ret;

	ret = platform_driver_register(&daqring_driver);
	if (ret)
		return ret;

	np = of_find_compatible_node(NULL, NULL, "jap,daqring");
	if (np) {
		of_node_put(np);
	} else {
		daqring_sim_pdev = platform_device_register_simple(
					DAQRING_NAME, -1, NULL, 0);
		if (IS_ERR(daqring_sim_pdev)) {
			ret = PTR_ERR(daqring_sim_pdev);
			platform_driver_unregister(&daqring_driver);
			return ret;
		}
	}
	return 0;
}

static void __exit daqring_exit(void)
{
	if (daqring_sim_pdev)
		platform_device_unregister(daqring_sim_pdev);
	platform_driver_unregister(&daqring_driver);
}

module_init(daqring_init);
module_exit(daqring_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joseph Ambrose Pagaran");
MODULE_DESCRIPTION("Simulated DAQ card: DT-probed platform driver, GPIO-loopback IRQ, mmap ring");
MODULE_VERSION("2.0");
