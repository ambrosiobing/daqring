# daqring — architecture, data paths, and where it goes next

Plain-text diagrams. They render anywhere: GitHub, a terminal, `less`,
an email to a colleague. Nothing here needs a rendering tool, which is
the point — a diagram you cannot read over SSH is a diagram you will not
read.

---

## 1. What daqring is, in one picture

```text
   USER SPACE
   ┌────────────────────────────────────────────────────────────────┐
   │  daqring_test            cat /sys/class/misc/daqring/*         │
   │  ┌──────────────┐        ┌───────────────────────────────┐     │
   │  │ read()/poll()│        │ mode  rate  produced  overruns│     │
   │  │   path       │        │ irq_latency  irq_latency_hist │     │
   │  └──────┬───────┘        └───────────────┬───────────────┘     │
   │         │                                │                     │
   │  ┌──────┴───────┐                        │                     │
   │  │  mmap() path │ ◄── zero copy ──┐      │                     │
   │  └──────────────┘                 │      │                     │
   └───────────│─────────────────────│─│──────│─────────────────────┘
        ioctl  │  read()             │ │      │ sysfs
   ═══════════════════════════════════════════════════════════════════
   KERNEL      ▼                     │ │      ▼
   ┌────────────────────────────────────────────────────────────────┐
   │  /dev/daqring  (misc device, dynamic minor)                    │
   │  ┌──────────┬──────────┬──────────┬───────────┬─────────────┐  │
   │  │  open    │  read    │  poll    │  ioctl    │   mmap      │  │
   │  └──────────┴────┬─────┴────┬─────┴─────┬─────┴──────┬──────┘  │
   │                  │          │           │            │         │
   │           bounce buffer  waitqueue   start/stop   remap_       │
   │           (copy_to_user  (wake from   rate/stats  vmalloc_     │
   │            outside lock)  threaded    reset       range        │
   │                  │        IRQ)        │           (read-only)  │
   │                  ▼                    ▼            │           │
   │  ┌──────────────────────────────────────────────┐  │           │
   │  │        struct daqring_dev                    │  │           │
   │  │  spinlock lock   ← the only lock the ISR takes│ │           │
   │  │  mutex cfg_lock  ← config paths only          │ │           │
   │  │  head, tail, overruns, latency stats          │ │           │
   │  └───────────────────┬──────────────────────────┘  │           │
   │                      ▼                             ▼           │
   │  ┌──────────────────────────────────────────────────────────┐  │
   │  │  vmalloc_user() window   ── mapped read-only into user ── │  │
   │  │  ┌────────────┬───────────────────────────────────────┐  │  │
   │  │  │ header page│  2730 sample slots (16 pages)         │  │  │
   │  │  │ head       │  [ seq | timestamp | ch | value ] ... │  │  │
   │  │  │ overruns   │                                       │  │  │
   │  │  └────────────┴───────────────────────────────────────┘  │  │
   │  └──────────────────────────────────────────────────────────┘  │
   │            ▲                                                   │
   │            │ writes a sample, publishes head (release)         │
   │  ┌─────────┴───────────┐        ┌──────────────────────────┐   │
   │  │ hardirq half        │        │ threaded half            │   │
   │  │ ktime_get_ns()      │───────►│ wake_up_interruptible()  │   │
   │  │ latency accounting  │ WAKE_  │                          │   │
   │  │ write ring slot     │ THREAD │                          │   │
   │  └─────────┬───────────┘        └──────────────────────────┘   │
   │            │ IRQ                                               │
   │  ┌─────────┴──────────┐   ┌────────────────────────────────┐   │
   │  │ platform_driver    │   │ hrtimer: pulses trigger GPIO   │   │
   │  │ probe() from DT    │   │ (the "sample clock")           │   │
   │  │ compatible =       │   └───────────────┬────────────────┘   │
   │  │  "jap,daqring"     │                   │                    │
   │  └────────────────────┘                   │                    │
   └───────────────────────────────────────────│────────────────────┘
   ═════════════════════════════════════════════════════════════════
   HARDWARE                                    ▼
   ┌────────────────────────────────────────────────────────────────┐
   │   BCM2837 GPIO block                                           │
   │                                                                │
   │      GPIO17 (pin 11) ──────── jumper wire ──────► GPIO27 (13)  │
   │      trigger out                                  irq in       │
   │           │                                          │         │
   │           └──── rising edge ─────────────────────────┘         │
   │                                                                │
   │   the whole "acquisition card" is one piece of wire            │
   └────────────────────────────────────────────────────────────────┘
```

---

## 2. What a real card looks like, and what daqring stands in for

```text
   REAL FPGA DAQ CARD                     daqring STAND-IN
   ══════════════════════                 ════════════════════════

   ┌──────────────────┐                   ┌──────────────────┐
   │ analogue front   │                   │  (none)          │
   │ end, ADC         │                   │                  │
   └────────┬─────────┘                   └──────────────────┘
            │ samples
   ┌────────▼─────────┐                   ┌──────────────────┐
   │ FPGA fabric      │                   │ hrtimer callback │
   │ FIFO, decimation │  ◄─── models ───► │ generates sample │
   │ timestamping     │                   │ + timestamp      │
   └────────┬─────────┘                   └────────┬─────────┘
            │ descriptor complete                  │ toggle GPIO
   ┌────────▼─────────┐                   ┌────────▼─────────┐
   │ DMA engine       │                   │ jumper wire      │
   │ writes to host   │  ◄─── models ───► │ edge → GPIO IRQ  │
   │ memory, raises   │                   │                  │
   │ MSI/legacy IRQ   │                   │                  │
   └────────┬─────────┘                   └────────┬─────────┘
            │ PCIe / AXI                           │ SoC GIC
   ┌────────▼─────────┐                   ┌────────▼─────────┐
   │ dma_alloc_       │                   │ vmalloc_user()   │
   │ coherent ring +  │  ◄─── models ───► │ ring + header    │
   │ descriptor       │                   │ page             │
   │ write-back       │                   │                  │
   └────────┬─────────┘                   └────────┬─────────┘
            │                                      │
            └────────► identical from here ◄───────┘
                              │
              char device, ioctl, mmap, sysfs,
              release/acquire head publication,
              never-stalling producer, counted overruns
```

**The honest boundary.** Everything below the dashed line is real and
transfers unchanged. Everything above it is a model. Say so before
anyone asks.

---

## 3. One sample, end to end (sequence)

```text
 hrtimer      GPIO17    wire    GPIO27/GIC   hardirq      ring      threaded    user
    │            │        │          │          │           │          │          │
    │ expires    │        │          │          │           │          │          │
    ├───────────►│ set 1  │          │          │           │          │          │
    │            ├────────┼─────────►│ edge     │           │          │          │
    │ t_toggle   │        │          ├─────────►│ IRQ       │          │          │
    │ recorded   │        │          │          │ t_isr     │          │          │
    │            │ set 0  │          │          │ = ktime   │          │          │
    │            │        │          │          │           │          │          │
    │            │        │          │  latency = t_isr − t_toggle     │          │
    │            │        │          │          ├──────────►│ write    │          │
    │            │        │          │          │           │ slot     │          │
    │            │        │          │          │           │ [seq,ts, │          │
    │            │        │          │          │           │  ch,val] │          │
    │            │        │          │          │ smp_store_release(head)         │
    │            │        │          │          ├──────────►│          │          │
    │            │        │          │          │ IRQ_WAKE_THREAD      │          │
    │            │        │          │          ├──────────────────────►│         │
    │            │        │          │          │           │          │ wake_up  │
    │            │        │          │          │           │          ├─────────►│
    │            │        │          │          │           │          │  read()  │
    │            │        │          │          │           │◄────────────────────┤
    │            │        │          │          │           │  or: acquire-load   │
    │            │        │          │          │           │  head from mmap     │
    │            │        │          │          │           │  (no syscall)       │
```

The hard-IRQ half does the minimum that must not be deferred:
timestamp, account, write the slot, publish. Everything that can wait —
waking sleepers — goes to the threaded half. That split is the same one
a real driver makes when it drains a card FIFO in the ISR and defers the
rest.

---

## 4. Structures and their relationships (UML-ish)

```text
        ┌────────────────────────────┐
        │   platform_driver          │
        │   .probe / .remove         │
        │   of_match_table:          │
        │     "jap,daqring"          │
        └─────────────┬──────────────┘
                      │ binds to
                      ▼
        ┌────────────────────────────┐         ┌───────────────────┐
        │   platform_device          │◄────────│ device tree node  │
        │   (from DT, or a bare      │  probed │ trigger-gpios     │
        │    one for simulation)     │  from   │ irq-gpios         │
        └─────────────┬──────────────┘         │ ring-pages        │
                      │ drvdata                └───────────────────┘
                      ▼
   ┌──────────────────────────────────────────────────┐
   │  struct daqring_dev                              │
   ├──────────────────────────────────────────────────┤
   │  shm, hdr, slots, capacity, shm_size             │──┐ owns
   │  hw_mode, trigger, irq_gpiod, irq, toggle_ns     │  │
   │  timer, period, rate_hz, running, noise          │  │
   │  head, tail, consumed, overruns, pulses          │  │
   │  lat_cnt/min/max/sum, lat_hist[8]                │  │
   │  lock (spinlock)   ── data path                  │  │
   │  cfg_lock (mutex)  ── config path                │  │
   │  waitq                                           │  │
   └───────┬──────────────────────┬───────────────────┘  │
           │ 1                    │ 1                    │
           ▼                      ▼                      ▼
   ┌───────────────┐   ┌────────────────────┐   ┌──────────────────┐
   │ miscdevice    │   │ hrtimer            │   │ vmalloc window   │
   │ /dev/daqring  │   │ CLOCK_MONOTONIC    │   │ hdr page + slots │
   │ fops, groups  │   │ → daqring_tick()   │   │ mapped read-only │
   └───────────────┘   └────────────────────┘   └──────────────────┘
           │
           │ exposes
           ▼
   ┌────────────────────────────────────────────────────────────┐
   │ sysfs: mode  sample_rate_hz  produced  overruns  running    │
   │        irq_latency  irq_latency_hist                        │
   └────────────────────────────────────────────────────────────┘
```

---

## 5. The mmap window, and the ring wrap

```text
   offset 0                     4096                        69632
   ├──────────────────────────────┼───────────────────────────┤
   │  header page                 │  2730 sample slots        │
   │  ┌────────────────────────┐  │  24 bytes each            │
   │  │ head        (u64)      │  │                           │
   │  │ overruns    (u64)      │  │  slot = head % capacity   │
   │  │ capacity    (u32)      │  │                           │
   │  │ sample_size (u32)      │  │                           │
   │  │ shm_bytes   (u32)      │  │                           │
   │  │ running     (u32)      │  │                           │
   │  └────────────────────────┘  │                           │

   producer never blocks; a slow reader is lapped:

        tail                head
         │                   │
    ┌────▼───────────────────▼──────────────────────────────┐
    │....|older|older|NEW|NEW|                              │   normal
    └───────────────────────────────────────────────────────┘

              head − tail  >  capacity   →   overrun
         ┌────────────────────────────────────────────────┐
         │ producer has wrapped past the reader's tail    │
         │ tail := head − capacity;  overruns++           │
         │ (flight-recorder semantics: keep the newest)   │
         └────────────────────────────────────────────────┘
```

---

## 6. Publishing a 64-bit head across a 32-bit boundary

The bug that appeared three times, in three disguises.

```text
   64-BIT HOST                          32-BIT HOST (armv7)
   ═══════════                          ═══════════════════

   producer:                            producer:
     smp_store_release(head, v)           smp_wmb()
     └─ one atomic 8-byte store           WRITE_ONCE(hi, upper_32(v))
                                          WRITE_ONCE(lo, lower_32(v))
                                          └─ hi first, then lo

   consumer:                            consumer:
     __atomic_load_n(head, ACQUIRE)       do {
     └─ one atomic 8-byte load               hi  = w[1]
                                             lo  = w[0]
                                             fence(ACQUIRE)
                                             hi2 = w[1]
                                          } while (hi != hi2)

   why it must be hand-rolled on 32-bit:

     smp_store_release(u64) ──► compile error: "Need native word
                                sized stores/loads for atomicity"

     u64 % capacity         ──► link error: no __aeabi_uldivmod
                                fix: div_u64_rem()

     __atomic_load_n(u64)   ──► libatomic may emit LDREXD/STREXD:
                                the STREXD is a WRITE, and the ring
                                is mapped PROT_READ  ──► SIGSEGV
                                fix: hi/lo read with retry
```

Only the low word can tear, and only at a 2³² wrap — and a torn value
can only read *forward*, which consumers already treat as an overrun and
resynchronise from. The invariant that makes this safe is that `head`
never decreases.

---

## 7. Driver state machine

```text
            insmod / probe
                  │
                  ▼
        ┌───────────────────┐
        │     STOPPED       │◄──────────────┐
        │  hrtimer idle     │               │
        │  IRQ armed (hw)   │               │
        └─────────┬─────────┘               │
                  │                         │
      IOC_START   │                         │  IOC_STOP
                  ▼                         │
        ┌───────────────────┐               │
        │     RUNNING       │───────────────┘
        │  producing        │
        └─────────┬─────────┘
                  │
      IOC_RESET   │  refused with -EBUSY while running
                  ✗

    IOC_RESET in STOPPED → zeroes head/tail/counters/histogram
    IOC_SET_RATE in either state → re-arms the timer if running
```

The refusal is deliberate: zeroing counters under a live producer would
race with the ISR, and `-EBUSY` is cheaper than a lock that the hot path
would have to take.

---

## 8. The stack, both directions

```text
   RUNTIME STACK                        BUILD STACK
   ═════════════                        ═══════════

   ┌───────────────────────┐            ┌────────────────────────┐
   │ daqring_test          │            │  ./go build            │
   │ (C, mmap + ioctl)     │            └───────────┬────────────┘
   ├───────────────────────┤                        ▼
   │ glibc / musl          │            ┌────────────────────────┐
   ├───────────────────────┤            │ Buildroot BR2_EXTERNAL │
   │ /dev/daqring, sysfs   │            │  package/daqring       │
   ├───────────────────────┤            │  rootfs-overlay        │
   │ daqring.ko            │            │  board/post-image.sh   │
   │  platform driver      │            │  configs/*.fragment    │
   │  misc device          │            └───────────┬────────────┘
   ├───────────────────────┤                        ▼
   │ Linux 4.19 / 6.1 /    │            ┌────────────────────────┐
   │ 6.18   (arm, arm64)   │            │ internal toolchain     │
   │  gpiolib, irq, hrtimer│            │ (gcc, binutils, libc)  │
   ├───────────────────────┤            ├────────────────────────┤
   │ device tree + overlay │            │ kernel + modules       │
   ├───────────────────────┤            ├────────────────────────┤
   │ start.elf / bootcode  │            │ busybox rootfs         │
   ├───────────────────────┤            ├────────────────────────┤
   │ BCM2837 (Cortex-A53)  │            │ genimage → sdcard.img  │
   └───────────────────────┘            └────────────────────────┘
```

---

## 9. Boot flow of the Buildroot image

```text
   power on
      │
      ▼
   ┌──────────────────┐   reads   ┌──────────────────────────────┐
   │ BCM2837 boot ROM │──────────►│ boot partition (FAT)         │
   └────────┬─────────┘           │  bootcode.bin, start.elf     │
            │                     │  config.txt ← dtoverlay=     │
            ▼                     │              daqring         │
   ┌──────────────────┐           │  bcm2710-rpi-3-b-plus.dtb    │
   │ firmware loads   │◄──────────│  overlays/daqring.dtbo       │
   │ DTB + overlay    │           │  zImage                      │
   └────────┬─────────┘           └──────────────────────────────┘
            │  merged device tree contains jap,daqring
            ▼
   ┌──────────────────┐
   │ Linux 6.1.61     │
   │ mounts rootfs    │
   └────────┬─────────┘
            ▼
   ┌──────────────────┐    modprobe daqring
   │ S99daqring       │───────────────────────┐
   └──────────────────┘                       ▼
                              ┌───────────────────────────────┐
                              │ probe(): DT node present?     │
                              │   yes → gpiod_get(trigger,irq)│
                              │         request_threaded_irq  │
                              │         → HARDWARE MODE       │
                              │   no  → hrtimer produces      │
                              │         → SIMULATION MODE     │
                              └───────────────────────────────┘
```

---

## 10. Measured: three kernels, one wire

```text
   average trigger-to-ISR latency (20 kHz, idle)
   0        2        4        6        8 µs
   ├────────┼────────┼────────┼────────┤
   4.19 32b ████████████████████ 4.2
   6.1  32b ██████████████████████████████████████████ 8.7 (at 2 kHz)
   6.18 64b █████████ 2.0

   worst case observed
   0       20       40       60 µs
   ├────────┼────────┼────────┤
   4.19 32b ████████████████████████████████████ 48.8
   6.1  32b ██████████ 13.5
   6.18 64b ██████████ 14.5

   edges lost (pulses − irqs)
   4.19 32b   22 in  99,996   ▓▓
   6.18 64b    0 in 100,011   ·
   6.18 64b    0 in 250,038 (at 50 kHz)   ·
```

The apparent interrupt-rate ceiling on 4.19 is largely absent on 6.18 —
the same board, the same wire. What looked like a hardware limit was
mostly a kernel one.

---

## 11. Where this goes: MVP → intermediate → advanced

The five responsibilities in Kontur's advert, and what each looks like
at three levels of ambition. The MVP column is what exists today.

```text
 ┌─────────────────┬──────────────────┬──────────────────┬──────────────────┐
 │ RESPONSIBILITY  │ MVP  (v3.0, done)│ INTERMEDIATE     │ ADVANCED         │
 ├─────────────────┼──────────────────┼──────────────────┼──────────────────┤
 │ Platform:       │ BR2_EXTERNAL,    │ CI builds image  │ signed images,   │
 │ Buildroot,      │ overlay + module │ on push; pinned  │ A/B rootfs with  │
 │ bootloader,     │ in image; boots  │ host toolchain   │ rollback; OTA    │
 │ DT, rootfs      │ on 3 B+          │ in a container   │ fleet update     │
 ├─────────────────┼──────────────────┼──────────────────┼──────────────────┤
 │ Drivers: DMA,   │ vmalloc ring;    │ dma_alloc_       │ scatter-gather   │
 │ IRQ, memory     │ GPIO IRQ;        │ coherent + real  │ descriptor ring; │
 │                 │ threaded split   │ dmaengine; MSI;  │ IOMMU; multi-    │
 │                 │                  │ PCIe or AXI      │ queue per core   │
 ├─────────────────┼──────────────────┼──────────────────┼──────────────────┤
 │ User-space      │ chardev, 5       │ shared library;  │ stable versioned │
 │ interfaces      │ ioctls, poll,    │ Python bindings; │ ABI; io_uring    │
 │                 │ RO mmap, sysfs   │ debugfs dumps    │ submission path  │
 ├─────────────────┼──────────────────┼──────────────────┼──────────────────┤
 │ Board bring-up  │ overlay written  │ two boards, two  │ JTAG + scope on  │
 │ & HW debugging  │ from scratch;    │ SoCs; ftrace and │ early revisions; │
 │                 │ latency histogram│ perf profiles    │ signal integrity │
 ├─────────────────┼──────────────────┼──────────────────┼──────────────────┤
 │ Network / data  │ (not built)      │ TCP streamer     │ AF_XDP or        │
 │ server, timing  │                  │ from the ring;   │ zero-copy NIC;   │
 │                 │                  │ NIC tuning, IRQ  │ PTP/IEEE-1588    │
 │                 │                  │ affinity         │ multi-sensor sync│
 └─────────────────┴──────────────────┴──────────────────┴──────────────────┘
```

### The intermediate step, drawn

```text
   today                          next
   ─────                          ────
   hrtimer ─► GPIO ─► IRQ         FPGA ─► DMA engine ─► IRQ (MSI)
       │                              │
       ▼                              ▼
   vmalloc ring                   dma_alloc_coherent ring
   (kernel writes each sample)    (device writes bursts, driver only
       │                           advances a descriptor pointer)
       ▼                              ▼
   one IRQ per sample             one IRQ per N samples
   ceiling ≈ 20–50 kHz            ceiling set by DMA bandwidth
```

The single most important change is the last line: batching. Every
measurement in this repo points at it.

### Advanced: where an accelerator would sit

```text
   ┌────────┐   DMA    ┌──────────────┐  zero-copy  ┌──────────────┐
   │ FPGA   ├─────────►│ coherent ring├────────────►│ user-space   │
   │ ADC    │          │ in DRAM      │   (mmap)    │ data server  │
   └────────┘          └──────┬───────┘             └──────┬───────┘
                              │                            │
                              │ same buffer, no copy       │ Ethernet
                              ▼                            ▼
                       ┌──────────────┐             ┌──────────────┐
                       │ edge AI      │             │ operator /   │
                       │ accelerator  │             │ recording    │
                       │ (NPU, Hailo, │             └──────────────┘
                       │  Coral, GPU) │
                       └──────┬───────┘
                              │ inference on the stream:
                              │  • classify GPR returns
                              │  • reject clutter before transmission
                              │  • trigger high-rate capture on interest
                              ▼
                       ┌──────────────┐
                       │ decisions,   │
                       │ not just data│
                       └──────────────┘
```

The interesting property is that the ring is already the right shape for
this: a read-only, zero-copy view of live samples is exactly what an
inference path wants, and it needs no new copy of the data. Inference
becomes a *second consumer* of the same buffer, and the flight-recorder
semantics mean a slow consumer degrades by dropping old data rather than
by stalling acquisition — which is the correct failure mode for an
instrument.

---

## 12. What is honestly missing

```text
   ┌──────────────────────────┬─────────────────────────────────────┐
   │ Not in this project      │ What would be needed                │
   ├──────────────────────────┼─────────────────────────────────────┤
   │ real DMA                 │ dma_alloc_coherent, dmaengine,      │
   │                          │ cache coherency on a non-coherent   │
   │                          │ bus, descriptor management          │
   │ PCIe                     │ enumeration, BAR mapping, MSI-X     │
   │ FPGA-side design         │ HDL, timing closure, FIFO depth     │
   │ upstream contribution    │ a patch series and review cycle     │
   │ high-rate networking     │ AF_XDP, NIC offload, IRQ affinity   │
   │ multi-sensor timing      │ PTP, hardware timestamping          │
   └──────────────────────────┴─────────────────────────────────────┘
```

Everything in the left column is a thing to learn, not a thing to claim.
The value of the right column is that each one is a concrete next step
rather than a vague aspiration — which is the difference between a gap
and a plan.
