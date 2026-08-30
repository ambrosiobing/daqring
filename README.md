# daqring — a simulated data-acquisition card driver

A small Linux kernel module that models the software side of an FPGA-based
data-acquisition add-on card, without needing the hardware. It exists to
demonstrate the pieces a real DAQ driver is made of:

| Real DAQ card                          | daqring stand-in                                    |
|----------------------------------------|-----------------------------------------------------|
| Sample-ready interrupt (ISR)           | `hrtimer` callback in hardirq context               |
| DMA ring buffer in coherent memory     | `vmalloc_user()` ring, mapped into user space       |
| Descriptor write-back / head pointer   | header page `head` field, published with `smp_store_release()` |
| Register access for control            | ioctls: start / stop / set rate / stats / reset     |
| Diagnostics                            | sysfs attributes under `/sys/class/misc/daqring/`   |

Samples are 24-byte timestamped words (sequence number, `CLOCK_MONOTONIC`
timestamp, channel, simulated 12-bit ADC value) produced at a configurable
rate up to 100 kHz.

## Two data paths

1. **`read()`/`poll()`** on `/dev/daqring` — blocking, copy-based. Samples
   are staged into a bounce buffer under the producer spinlock (bounded to
   256 per call), then `copy_to_user()` runs outside the lock, since it may
   fault and sleep.
2. **`mmap()`** — zero-copy. Page 0 of the mapping is a header the
   "hardware" writes back to; the consumer loads `head` with acquire
   semantics (pairing with the producer's release store), then indexes the
   sample slots directly. The mapping is read-only and `VM_MAYWRITE` is
   cleared so `mprotect()` cannot upgrade it.

The producer never stalls — a consumer that falls a full ring behind gets
lapped and the loss is counted in `overruns`, which is how real acquisition
hardware behaves.

## Locking model

- `lock` (IRQ-safe spinlock): the only lock the timer callback takes;
  guards head/tail/counters.
- `cfg_lock` (mutex): serialises configuration (rate changes, start/stop,
  reset). Reset is refused with `-EBUSY` while acquisition runs.

## Building — Raspberry Pi (ARM Linux)

```sh
# Raspberry Pi OS (Bookworm or later):
sudo apt update && sudo apt install -y build-essential linux-headers-$(uname -r) \
  || sudo apt install -y raspberrypi-kernel-headers   # older Raspberry Pi OS

git clone <this-repo> && cd daqring
make            # builds daqring.ko and test/daqring_test
./scripts/demo.sh
```

Any other Linux with headers for the running kernel works the same way
(`make`, `sudo insmod daqring.ko`). Version guards cover the
`hrtimer_setup()` (≥ 6.13) and `vm_flags_clear()` (≥ 6.3) API changes.

## Using it

```sh
sudo insmod daqring.ko                 # optional: ring_pages=64
sudo ./test/daqring_test read 5000 3   # poll+read path, 5 kHz for 3 s
sudo ./test/daqring_test mmap 20000 3  # zero-copy path, 20 kHz for 3 s
cat /sys/class/misc/daqring/produced /sys/class/misc/daqring/overruns
echo 250 | sudo tee /sys/class/misc/daqring/sample_rate_hz
sudo rmmod daqring
```

The test client verifies sequence continuity on every sample and reports
gaps alongside the kernel's own counters, so lost data is visible, not
silent.

## What a real-hardware version would add

- `platform_driver` probe bound by device tree compatible string, `devm_*`
  resource management
- a real IRQ handler (`request_irq`) split into hardirq + threaded halves
- `dma_alloc_coherent()`/dmaengine descriptors instead of the vmalloc ring
- runtime PM, and `debugfs` for register dumps

## License

GPL-2.0. Author: Joseph Ambrose Pagaran.
