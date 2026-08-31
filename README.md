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

## v2: device tree, platform driver, real interrupts

Since v2 the module is a **platform driver probed from the device
tree**, and the "card" is real hardware in miniature: a jumper wire
from GPIO17 (pin 11) to GPIO27 (pin 13). An hrtimer pulses the trigger
output at the sample rate; the looped-back edge arrives as a **genuine
hardware interrupt** through the SoC's GPIO block. The hardirq half
timestamps the edge, updates a trigger-to-ISR **latency histogram**
(`irq_latency`, `irq_latency_hist` in sysfs) and writes the sample —
the equivalent of draining a card FIFO in the ISR; the threaded half
(`request_threaded_irq`) wakes sleeping readers.

The [overlay](overlays/daqring-overlay.dts) declares the node
(`compatible = "jap,daqring"`, `trigger-gpios`, `irq-gpios`,
`ring-pages`). Without it — no overlay loaded, no wire, or a machine
with no GPIOs at all — probe falls back to **simulation mode** (v1
behaviour, tagged `v1.0`): the driver registers a bare platform device
so it binds by name and the hrtimer produces samples directly. The
active mode is reported in `dmesg` and `/sys/class/misc/daqring/mode`.

Install on a Raspberry Pi:

```sh
sudo apt install -y device-tree-compiler   # if dtc is missing
make dtbo && make install-overlay          # copies .dtbo, edits config.txt
sudo reboot                                # loader applies the overlay
make && sudo insmod daqring.ko             # dmesg: "ready: hardware mode"
```

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

## Measured baseline — Raspberry Pi 3 B+ (31 Aug 2026)

First hardware run: Raspberry Pi 3 B+ (Cortex-A53), Raspbian with the
**32-bit** `4.19.66-v7+` kernel — deliberately kept as a harder
portability target than a modern 64-bit image. Ring: 2730 slots,
16 pages + header page (69,632-byte mmap window).

| Path              | Requested rate | Achieved                              | Sequence gaps |
|-------------------|----------------|---------------------------------------|---------------|
| `read()`/`poll()` | 5 kHz, 3 s     | 15,002 samples, 5,001 samples/s       | 0             |
| `mmap` zero-copy  | 20 kHz, 3 s    | 59,997 samples, 19,998 samples/s      | 0             |

Kernel counters after the run: `produced=75,019`, read-path
`consumed=15,002`, `overruns=57,287`. The overruns are *expected and
correct*: they count the read()-path tail being lapped while the mmap
test ran with no read() consumer draining it — the flight-recorder
semantics doing exactly what they should. The mmap consumer itself saw
zero gaps at 20 kHz.

Bringing the module up on this target surfaced two genuine 32-bit ARM
portability bugs, both fixed in commit `8ae2af7`: `smp_store_release()`
of the 64-bit head is rejected on armv7 (no native 8-byte atomic
store), replaced by an ordered high-word/low-word publish; and `u64 %`
does not link on 32-bit ARM kernels (no `__aeabi_uldivmod`), replaced
by `div_u64_rem()`.

## What a real-hardware version would add

- `platform_driver` probe bound by device tree compatible string, `devm_*`
  resource management
- a real IRQ handler (`request_irq`) split into hardirq + threaded halves
- `dma_alloc_coherent()`/dmaengine descriptors instead of the vmalloc ring
- runtime PM, and `debugfs` for register dumps

## Design lineage and prior art

Nothing here is invented from scratch — each mechanism follows an
established, documented pattern from production kernel infrastructure or
published DAQ work:

- **Char device + ioctl + mmap + wait-queue blocking I/O** — the
  canonical structure from J. Corbet, A. Rubini, G. Kroah-Hartman,
  [*Linux Device Drivers*, 3rd ed.](https://lwn.net/Kernel/LDD3/),
  O'Reilly, 2005 (chs. 3, 6, 15).
- **Header page with a release/acquire-published `head` pointer** —
  modelled on the Linux **perf ring buffer**, whose
  `perf_event_mmap_page::data_head` is published to a read-only
  user-space mapping with exactly this barrier pairing
  ([`perf_event_open(2)`](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)),
  and on **io_uring**'s mmap'd submission/completion rings — J. Axboe,
  [*Efficient IO with io_uring*](https://kernel.dk/io_uring.pdf), 2019.
- **Lockless ring-buffer theory** — M. Desnoyers, M. R. Dagenais,
  [“Lockless multi-core high-throughput buffering scheme for kernel
  tracing”](https://dl.acm.org/doi/10.1145/2421648.2421659), *ACM SIGOPS
  Operating Systems Review* 46(3), 2012 (formally verified in ch. 5 of
  Desnoyers' PhD dissertation, École Polytechnique de Montréal, 2009);
  see also the kernel's own
  [ftrace ring-buffer design document](https://docs.kernel.org/trace/ring-buffer-design.html).
- **Never-stalling producer that laps slow consumers, with counted
  overruns** — LTTng's *flight-recorder* / ftrace *overwrite* mode
  (same references as above); this is also how real acquisition
  hardware behaves, since an ADC cannot be backpressured.
- **The overall DAQ-driver shape (timestamped blocks, sysfs control,
  trigger/start semantics)** — F. Vaga, A. Rubini, J. D. González Cobas
  *et al.*, [“ZIO: The Ultimate Linux I/O
  Framework”](https://proceedings.jacow.org/ICALEPCS2013/papers/momib09.pdf),
  *Proc. ICALEPCS 2013*, CERN
  ([CDS record](https://cds.cern.ch/record/1620805)). daqring is in
  effect a single-device miniature of what ZIO generalises.
- **Zero-copy FPGA→user-space delivery through a char device** —
  [bperez77/xilinx_axidma](https://github.com/bperez77/xilinx_axidma)
  (zero-copy AXI DMA driver + user library for Zynq) and
  [ikwzm/udmabuf](https://github.com/ikwzm/udmabuf) (DT-probed,
  mmap-able DMA buffers). The copy-vs-mmap trade-off that motivates
  keeping *both* data paths is measured in the REDS institute's
  [Zynq DMA benchmark](https://blog.reds.ch/?p=2054).
- **DMA engines and their Linux drivers for FPGA DAQ** —
  W. M. Zabołotny, [“DMA implementations for FPGA-based data
  acquisition
  systems”](https://www.spiedigitallibrary.org/conference-proceedings-of-spie/10445/1/DMA-implementations-for-FPGA-based-data-acquisition-systems/10.1117/12.2280937.short),
  *Proc. SPIE* 10445, 2017; for the Ethernet-streaming stretch goal,
  his [“Low latency protocol for transmission of measurement data from
  FPGA to Linux computer via 10 Gbps Ethernet
  link”](https://arxiv.org/abs/1503.06871), 2015.
- **Developing DAQ drivers before/without the hardware** —
  [“QEMU-based hardware/software co-development for DAQ
  systems”](https://arxiv.org/abs/2109.14735), 2021, argues the same
  workflow this module embodies; daqring trades emulator fidelity for
  zero-setup simulation inside the driver itself.
- **Hardirq/threaded-IRQ split (v2, real GPIO interrupts)** — J. Corbet,
  [“Moving interrupts to threads”](https://lwn.net/Articles/302043/),
  LWN.net, 2008.

## License

GPL-2.0. Author: Joseph Ambrose Pagaran.
