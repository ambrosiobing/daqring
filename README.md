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

## Quick start

Everything runs through one self-updating entry point, so there is
never a long command to type:

```sh
./go            # list the tasks
./go demo       # build and run the two-path demo
./go char       # rate sweep + latency under load
./go overlay    # compile and install the device-tree overlay
./go build      # start the Buildroot image build
./go status     # how that build is doing
```

Each task pulls the latest repo first. First time on a new machine:

```sh
git clone https://github.com/ambrosiobing/daqring.git && cd daqring && ./go
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for plain-text
diagrams: the layered architecture, a real card mapped onto its
stand-in, the per-sample sequence, the ring and its wrap, the 64-bit
publish across a 32-bit boundary, the boot flow, and an MVP →
intermediate → advanced roadmap against each of the five platform
responsibilities.

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

## Measured results — v2 hardware mode (Pi 3 B+, 31 Aug 2026)

Same board and kernel as the baseline below, now with the overlay
loaded and every sample produced by a real GPIO interrupt (jumper
loopback GPIO17→GPIO27), demo at 5 kHz (read path) then 20 kHz (mmap):

**Trigger-to-ISR latency** over 75,045 interrupts, stock (non-RT)
`4.19.66-v7+` kernel with the desktop running:

| min | avg | max |
|-----|-----|-----|
| 2.0 µs | 4.2 µs | 48.8 µs |

| bucket | ≤5 µs | ≤10 µs | ≤20 µs | ≤50 µs | >50 µs |
|--------|-------|--------|--------|--------|--------|
| count  | 72,526 | 2,300 | 201 | 18 | 0 |

**Edge accounting:** `pulses=75047`, `irqs=75045` — two edges lost in
75k at rates up to 20 kHz (0.003%), the honest cost of one interrupt
per sample; a real card batches samples per IRQ precisely to push this
ceiling out. Both data paths again completed with **zero sequence
gaps** (read: 5,001 samples/s; mmap: 19,997 samples/s).

## Characterisation — rate sweep and behaviour under load

Pi 3 B+, 32-bit `4.19.66-v7+`, hardware mode, 5 s per point, module
reloaded between runs so each row stands alone. `pulses` is trigger
edges emitted, `irqs` is interrupts actually taken; the gap between
them is the interrupt-rate ceiling showing itself. Latency is
trigger-to-ISR, in microseconds.

| Sample rate | Achieved | Seq. gaps | Pulses | IRQs | Lost | min | avg | max |
|---|---|---|---|---|---|---|---|---|
| 5 kHz  | 5,000/s  | 0 | 25,003  | 25,003  | 0    | 1.6 | 4.2 | 80.4 |
| 10 kHz | 10,000/s | 0 | 50,005  | 50,005  | 0    | 2.9 | 7.3 | 22.3 |
| 20 kHz | 19,990/s | 0 | 99,996  | 99,974  | 22   | 0.9 | 4.3 | 64.7 |
| 50 kHz | 49,984/s | 0 | 250,064 | 250,027 | 37   | 2.5 | 3.8 | 26.6 |
| 20 kHz, 4 cores loaded | 19,999/s | 0 | 100,007 | 100,007 | **0** | 1.0 | **1.4** | **9.3** |

Two things worth drawing out.

**The requested rate holds to 50 kHz** — the achieved rate tracks it to
within 0.03%, and the mmap consumer sees zero sequence gaps at every
point. What degrades first is not throughput but *edge capture*: from
20 kHz upward a small fraction of trigger edges (0.015–0.02%) never
become interrupts. One interrupt per sample is the wrong shape at
these rates, which is precisely why real acquisition hardware batches
many samples per interrupt and hands them over by DMA.

**Latency improves under load, and edge loss disappears.** Average
trigger-to-ISR latency falls from 4.3 µs to 1.4 µs and the worst case
from 64.7 µs to 9.3 µs when all four cores are busy. That is not a
measurement artefact: an idle Cortex-A53 sits in WFI at a reduced
clock, and pulling it out of that state costs microseconds, while a
loaded CPU is already awake at full frequency. The practical lesson
for a low-latency acquisition box is the familiar one — pin the
`performance` governor and keep the CPU out of deep idle states,
because *idle* is what makes interrupt response slow and jittery.

## What a real-hardware version would add

- `platform_driver` probe bound by device tree compatible string, `devm_*`
  resource management
- a real IRQ handler (`request_irq`) split into hardirq + threaded halves
- `dma_alloc_coherent()`/dmaengine descriptors instead of the vmalloc ring
- runtime PM, and `debugfs` for register dumps

## Buildroot: the module as part of a bootable image

[`br2-external/`](br2-external/) is a `BR2_EXTERNAL` tree that builds
daqring into a complete Linux image: the module is compiled against the
Buildroot kernel by the `kernel-module` infrastructure, `daqring_test`
is cross-compiled into `/usr/bin`, and a rootfs-overlay init script
loads the module at boot. The result is an SD/USB image whose OS,
kernel, driver and test client all come out of one reproducible build.

Buildroot is a cross-build system: run it on a Linux workstation (or
WSL2 on Windows). It builds its own toolchain from source, so on a
workstation expect roughly an hour, and on a Pi 3 expect the better
part of a day.

```sh
sudo apt update && sudo apt install -y git
git clone https://github.com/ambrosiobing/daqring.git
cd daqring && ./scripts/day2.sh
```

`day2.sh` installs the prerequisites and launches the build detached,
scaling parallelism to the host (about one job per GB of RAM, capped at
core count). Follow it at any time with:

```sh
./scripts/status.sh
```

The result is `~/br/buildroot-*/output/images/sdcard.img`. Write it to
a **USB stick** - both the Pi 3 B+ and Pi 4 B boot from USB, so the
development SD card is never touched. On Linux use the guarded helper,
which refuses to write to an SD-card device or to anything holding a
mounted root/boot filesystem, and asks for confirmation:

```sh
sudo ./scripts/flash-image.sh /dev/sdX
```

On Windows, use Raspberry Pi Imager's *Use custom* option instead.

## Running from the Buildroot image (Pi 3 B+, 1 Sep 2026)

The image boots, loads the driver from its own init script, and comes
up in **hardware** mode - the overlay in the boot partition is found,
the platform driver binds to the `jap,daqring` node, and the interrupt
is armed before login:

```
daqring daqring: ready: hardware mode, 2730 slots (16 pages), default 1000 Hz, irq armed
Welcome to Buildroot
buildroot login: root
# cat /sys/class/misc/daqring/mode
hardware
# daqring_test mmap 2000 3
mapped 69632 bytes, 2730 slots
consumed 5999 samples in 3.00 s (2000 samples/s), 0 sequence gaps
kernel:  produced=6015 consumed=0 overruns=3285 rate=2000 Hz
# cat /sys/class/misc/daqring/irq_latency
pulses=6019 irqs=6019 min_ns=4791 avg_ns=8696 max_ns=13490
```

Everything in that transcript - bootloader, kernel 6.1.61, root
filesystem, the module, the overlay and `daqring_test` - comes from one
`./go build`.

Two things worth comparing against the Raspbian numbers above, on the
same board and the same wire:

| | Raspbian, 4.19.66 | Buildroot, 6.1.61 |
|---|---|---|
| avg trigger-to-ISR | 4.2 µs | 8.7 µs |
| worst case | 48.8 µs | **13.5 µs** |
| edges lost | 2 in 75,045 | **0 in 6,019** |

The Buildroot image is slower on average but far tighter at the tail -
a 3.6× lower worst case. For acquisition work that is the better
trade: jitter is what costs you samples, not average latency. A minimal
image has almost nothing else contending for the CPU, which is exactly
why instruments ship a stripped root filesystem rather than a desktop.

This also confirmed the user-space fix for the read-only 64-bit head
load: 2 kHz is the rate that used to segfault, and it now runs with
`pulses == irqs` and zero sequence gaps.

**Note on booting:** USB-stick boot did not work on this Pi 3 B+ (red
LED, no green activity - the boot ROM's USB mass-storage window is
short and many drives miss it), so the image was written to the
microSD card instead. The image itself is identical either way.

## Three kernels, one wire (Pi 3 B+)

The same driver, overlay and jumper wire, measured on three different
kernels spanning six years and both word sizes. Nothing in the source
changes between them; the version guards and the 32/64-bit paths are
selected at build time.

**Raspberry Pi OS Lite 64-bit, `6.18.34+rpt-rpi-v8`, hardware mode**, 5 s
per point:

| Rate | Achieved | Gaps | Pulses | IRQs | Lost | min | avg | max |
|---|---|---|---|---|---|---|---|---|
| 1 kHz  | 1,000/s  | 0 | 5,000   | 5,000   | 0 | 1.46 | 2.76 | 16.0 |
| 5 kHz  | 5,000/s  | 0 | 25,002  | 25,002  | 0 | 1.41 | 2.11 | 25.1 |
| 10 kHz | 9,999/s  | 0 | 50,004  | 50,004  | 0 | 1.25 | 2.05 | 16.2 |
| 20 kHz | 19,999/s | 0 | 100,011 | 100,011 | 0 | 1.30 | 2.00 | 14.5 |
| 50 kHz | 49,997/s | 0 | 250,038 | 250,038 | 0 | 1.30 | 1.91 | 12.2 |
| 20 kHz, 4 cores loaded | 19,999/s | 0 | — | = pulses | 0 | 1.30 | 2.33 | 13.7 |

(Board reported 63.4 °C and `throttled=0x80008` — the soft temperature
limit was active, normal for a 3 B+ above 60 °C and worth knowing when
reading the tail latencies.)

A separate 20 kHz demo run on 6.18 over **75,018 interrupts, none
lost**, gives the distribution rather than just the extremes:

| ≤5 µs | ≤10 µs | ≤20 µs | ≤50 µs | >50 µs |
|---|---|---|---|---|
| 65,826 (87.7%) | 8,438 | 752 | 2 | **0** |

min 1.46 µs, avg 3.42 µs, max 32.2 µs — the worst case moves between
runs (12–32 µs) while the bulk of the distribution barely shifts, which
is the usual signature of occasional contention rather than a
systematic cost.

Compared at 20 kHz idle, the same board and wire:

| Kernel | Word size | avg | max | Edges lost |
|---|---|---|---|---|
| 4.19.66 (Raspbian, desktop) | 32-bit | 4.3 µs | 64.7 µs | 22 in 99,996 |
| 6.18.34 (Raspberry Pi OS Lite) | 64-bit | **2.0 µs** | **14.5 µs** | **0 in 100,011** |

The Buildroot 6.1.61 image sits between them on average (8.7 µs at
2 kHz — a different operating point, so not directly comparable) but
shares the modern kernels' tight tail: 13.5 µs worst case.

Two things worth taking from this. **The interrupt-per-sample ceiling
is not a property of the hardware alone** - on 4.19 a fraction of edges
went missing above 20 kHz, and on 6.18 the same board at the same rates
loses none, so what looked like a hardware limit was largely a kernel
one. And **the worst case improved by 4.5×** across the kernel jump
while the average only halved, which is the number that decides whether
an acquisition system drops samples.

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
