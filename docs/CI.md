# Continuous integration and delivery for daqring

How every change to this repository is built, tested and — when tagged —
turned into a bootable image, without a person doing anything but
pushing. Plain-text diagrams throughout; they render on GitHub, in a
terminal, and in a PDF.

---

## 1. The questions this answers

| Question | Short answer |
|---|---|
| Does it include unit tests? | **Yes.** `test/unit_ring.c`: ABI layout, ring arithmetic, and the head-publication protocol under real concurrency across the 2³² wrap. Runs in seconds, no hardware, no root. |
| Does it check and test automatically? | **Yes.** Every push and every pull request runs lint → unit → module-load-and-run on x86-64 *and* ARM64. |
| If it fails, is the change rejected? | **Yes, with branch protection on.** A pull request cannot be merged until the required checks are green (§5). Without branch protection CI still runs and reports, but nothing enforces. |
| Otherwise merged into master? | **Yes** — merged by a person clicking the button, or automatically if auto-merge is enabled. CI never merges on its own initiative; it *permits* merging. |
| Python or C/C++ for GitHub Actions? | **Neither.** Workflows are YAML; steps are shell. The code under test is C, so the tests are C. Python has no role here — see §7. |
| Do I need a Jenkins server? | **No.** Hosted GitHub Actions is free for public repositories and needs no server. Jenkins is for when you must self-host everything — see §8. |
| What can CI *not* test? | **Timing.** The hardware *code path* — gpiod, the IRQ, the ISR — runs in CI on a simulated gpiochip (`hal` job, §4b). What no hosted runner can measure is real latency, jitter and lost edges on a real SoC; that is the self-hosted Pi (§4). |
| HAL-level tests without hardware? | **Yes** — the kernel's `gpio-sim` provides a simulated chip whose lines raise real interrupts; the driver is pointed at it by module parameters and 500 user-space edges must become exactly 500 interrupts (§4b). |
| QEMU or similar? | **Not yet, deliberately.** It would add an image boot test and, for a real card, a register-map model; both are explained and costed in §4b. Renode over QEMU if a card model is ever the goal. |

---

## 2. The pipeline, end to end

```text
   developer
      │  git push / open PR
      ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │  ci.yml  (every push, every pull request)          ~5 minutes   │
   │                                                                 │
   │   ┌────────┐     ┌────────┐     ┌────────────────────────────┐  │
   │   │  lint  │     │  unit  │──┬─►│  module  (x86-64)          │  │
   │   │        │     │        │  │  │  module  (ARM64)           │  │
   │   │ dtc    │     │ ABI    │  │  │  build .ko against the     │  │
   │   │ shell- │     │ ring   │  │  │  runner's kernel           │  │
   │   │  check │     │ seqlock│  │  │  insmod → simulation mode  │  │
   │   │ cppchk │     │ race   │  │  │  read() + mmap: 0 gaps     │  │
   │   │ check- │     │        │  │  │  rmmod, dmesg clean        │  │
   │   │  patch │     │        │  │  └─────────────┬──────────────┘  │
   │   └───┬────┘     └───┬────┘  │  ┌─────────────┴──────────────┐  │
   │       │              │       └─►│  hal  (gpio-sim)           │  │
   │       │              │          │  simulated gpiochip, module│  │
   │       │              │          │  in HARDWARE mode, 500     │  │
   │       │              │          │  edges → 500 IRQs, 0 gaps  │  │
   │       │              │          └─────────────┬──────────────┘  │
   │       └──────────────┴────────────────────────┘                 │
   │                              │ all green                        │
   └──────────────────────────────┼──────────────────────────────────┘
                                  ▼
                    ┌─────────────────────────────┐
                    │ branch protection on master │
                    │ required checks: lint,      │
                    │ unit, module (x86-64),      │
                    │ module (arm64), hal         │
                    └─────────────┬───────────────┘
                     red: merge   │   green: merge
                     button       │   button enabled
                     disabled     ▼
                            master updated
                                  │
              ┌───────────────────┼───────────────────────┐
              │ git tag v*        │ Mondays 03:17 / manual │ manual, on the bench
              ▼                   ▼                        ▼
   ┌──────────────────────────────────────┐   ┌─────────────────────────────┐
   │  image.yml           ~2 hours        │   │  hil.yml   (self-hosted Pi) │
   │                                      │   │                             │
   │  free 25 GB · restore download cache │   │  build against Pi kernel    │
   │  ./scripts/buildroot-overnight.sh    │   │  insmod → HARDWARE mode     │
   │  ./scripts/verify-image.sh           │   │  rate sweep 1–50 kHz        │
   │  upload sdcard.img (30 days)         │   │  latency under load         │
   │  on tag: attach to GitHub Release    │   │  assert pulses == irqs      │
   └──────────────────────────────────────┘   └─────────────────────────────┘
```

Three workflows, three cadences, one principle: **the fast gate runs on
everything; the slow and the hardware-bound run when they are worth
running.** A pull-request gate that takes two hours is a gate people
learn to route around.

---

## 3. What each check actually verifies

```text
 ┌──────────┬────────────────────────────────────┬─────────────┬──────────┐
 │ CHECK    │ WHAT IT PROVES                     │ WHERE       │ BLOCKS?  │
 ├──────────┼────────────────────────────────────┼─────────────┼──────────┤
 │ dtc      │ the device-tree overlay compiles   │ lint        │ yes      │
 │ shellchk │ scripts have no shell errors       │ lint        │ errors   │
 │ cppcheck │ user-space C has no static defects │ lint        │ yes      │
 │ checkpat │ kernel coding style                │ lint        │ advisory │
 │ unit     │ ABI layout is exactly as documented│ unit        │ yes      │
 │          │ slot/lap arithmetic is right       │             │          │
 │          │ head protocol survives 2^32 wrap   │             │          │
 │          │   under a real racing thread       │             │          │
 │ module   │ .ko builds on a current kernel     │ x86-64,     │ yes      │
 │          │ loads, probes (simulation), runs   │  arm64      │          │
 │          │ both data paths lose nothing       │             │          │
 │          │ unloads with a clean dmesg         │             │          │
 │ hal      │ the HARDWARE path with no hardware:│ hal         │ yes      │
 │          │ gpiod, request_threaded_irq, ISR,  │ (gpio-sim)  │ (skips   │
 │          │ 500 user-space edges → 500 IRQs    │             │  if no   │
 │          │ and 0 gaps on a simulated gpiochip │             │  gpio-sim)│
 │ image    │ the whole image reproduces from    │ image.yml   │ release  │
 │          │ one command; overlay + module in it│             │          │
 │ hil      │ hardware mode; no lost edges 1–50k │ Pi          │ manual   │
 └──────────┴────────────────────────────────────┴─────────────┴──────────┘
```

**Why checkpatch is advisory.** Kernel coding style is a *goal* for
out-of-tree code, not a contract; blocking merges on a line-length
warning teaches people to disable the check. It is reported on every
run and read when it matters — before sending anything upstream.

**Why the module job is the important one.** Unit tests cover the
logic that can be lifted into user space. The kernel module itself is
only proven by building it against a real kernel, loading it, and
running data through it. That the runner has no GPIO is exactly why
the driver's simulation-mode fallback exists: the same `.ko` that takes
real interrupts on the Pi runs its full data path on a cloud VM.

---

## 4. Three tiers, because hardware does not live in the cloud

```text
   TIER 1  hosted runners            TIER 2  self-hosted bench     TIER 3  heavy
   ───────────────────────           ───────────────────────────   ─────────────
   every push, minutes               on demand, minutes            weekly / tag
   lint · unit · module (sim)        module (HARDWARE) · sweep     Buildroot image
   x86-64 + ARM64                    Raspberry Pi, jumper wire     hosted, 2 h
   free                              a Pi and a wire               free

   what it cannot do:                what it adds:                 what it adds:
   touch a GPIO                      real interrupts,              the deliverable
                                     real latency                  artefact
```

Tier 2 is a Raspberry Pi registered as a GitHub self-hosted runner —
three commands from *Settings → Actions → Runners*, then a service so
it survives reboots. `hil.yml` targets the label `pi` and is
manual-trigger only, so a Pi that is powered off never blocks anyone.

This is the shape CI takes at any company whose product is an
instrument: everything that needs no hardware runs on cheap hosted
machines, and one bench machine with the real card does the rest.
Kontur's advert calls it *"automated deployment"*; this is what that
looks like in practice.

---

## 4b. The testing ladder: unit, HAL, emulation, hardware

"Can this be tested without the hardware?" has four different answers
depending on what *this* is. Each rung proves something the one below
cannot, and costs more to climb.

```text
   rung   what runs                  what it proves                    cost      here
   ────   ─────────────────────────  ────────────────────────────────  ────────  ──────────
    4     real board + real wire     the physics: latency, jitter,     a Pi on   hil.yml
          (HIL)                      lost edges, thermal, the SoC's    the bench (manual)
                                     GPIO block, the actual overlay
    3     emulated SoC               boot chain, kernel config, DTB,   hours to  not built
          (QEMU / Renode)            init scripts - the *image*, not   set up    (see below)
                                     the timing
    2     simulated peripheral       gpiod, request_threaded_irq,      minutes   ci.yml
          (gpio-sim, "HAL" level)    the ISR, the ring - the driver's  free      `hal` job
                                     hardware path, with fake edges
    1     user-space unit tests      ABI layout, arithmetic, the       seconds   ci.yml
                                     head protocol under a race        free      `unit` job
    0     build + load, sim mode     it compiles on this kernel and    minutes   ci.yml
                                     the data paths lose nothing       free      `module` job
```

### Rung 2 — the HAL test, and why it exists

There is no formal "HAL" in a Linux driver the way there is in
microcontroller firmware; the equivalent seam is **gpiolib and the IRQ
core**. The driver asks for two lines by name (`trigger`, `irq`) and an
interrupt on one of them, and does not care what is behind them.

The kernel ships a simulated GPIO chip for exactly this purpose:
`gpio-sim`, configured through configfs, whose lines can be driven from
user space and which raises real edge interrupts through the ordinary
IRQ machinery. So the `hal` job does this:

```text
   configfs                          kernel                        driver
   ────────                          ──────                        ──────
   mkdir gpio-sim/daq/bank0
   num_lines = 2, label = daqring-sim
   live = 1                  ──────► gpiochip "daqring-sim"
                                     lines 0 and 1

   insmod daqring.ko gpiochip=daqring-sim trigger_line=0 irq_line=1
                                     lookup table:                 probe():
                                       "trigger" → line 0  ──────►  gpiod_get("trigger")  ✓
                                       "irq"     → line 1  ──────►  gpiod_get("irq")      ✓
                                                                     gpiod_to_irq()        ✓
                                                                     request_threaded_irq ✓
                                                                     mode = HARDWARE

   echo pull-up   > sim_gpio1/pull ─► edge ─► IRQ ─► hardirq half: timestamp, write slot
   echo pull-down > sim_gpio1/pull                   threaded half: wake readers
        × 500                                       user space: read() 500 samples, 0 gaps
                                                    sysfs: irqs=500     ← the assertion
```

What it proves: the entire hardware code path — GPIO acquisition,
interrupt registration, the hard/threaded split, the ring, the wake-up,
the counters — on a cloud VM with no pins. What it cannot prove:
anything about *timing*. The latency numbers it produces are
meaningless, because the "trigger" the hrtimer pulses is not wired to
the "irq" line; the edges come from a shell loop. Timing is rung 4's
job and nothing lower can do it.

This is also the honest answer to "how do you develop a driver before
the card exists?": the same lookup-table mechanism that points at
`gpio-sim` today points at a real FPGA's lines tomorrow, and nothing in
`probe()` changes.

### Rung 3 — QEMU and Renode, and why they are not here (yet)

An emulator sits between the simulated peripheral and the real board.
Two things it would add:

- **A boot test of the actual image.** `qemu-system-aarch64 -M raspi3b`
  can boot a Raspberry Pi kernel and root filesystem, which would turn
  "the image builds" into "the image boots to a login prompt and
  `S99daqring` loaded the module". That is a genuine gap in this
  pipeline: `image.yml` verifies the image's *contents*, not that it
  boots.
- **A model of the card.** For a real FPGA card the mature practice is
  a QEMU device model of its register map, so the driver is developed
  and regression-tested against the model before and alongside the
  silicon — the approach in *QEMU-based hardware/software co-development
  for DAQ systems* (arXiv:2109.14735), cited in the README.

Why it is not built here: the first needs a day of work to get a Pi 3
image booting under QEMU reliably (the machine model, DTB and kernel
must agree, and the 32-bit-kernel-on-raspi3b case is fiddly), and even
then QEMU's BCM2837 GPIO model does not deliver edge interrupts the way
the silicon does — so it would not replace rung 2, only rung 0 for the
image. The second is a project in itself, and only worth it for a card
that exists. **Renode** is the purpose-built alternative — designed for
peripheral models and multi-node simulation, with a testing DSL — and
is what I would reach for over QEMU if a card model were the goal.

The pragmatic call for a project this size: rung 2 gives most of the
value of rung 3 at a hundredth of the cost, and rung 4 is a Pi and a
wire. QEMU earns its place the day the image boot test is worth a day.

### Rung 1½ — KUnit, for the purist

The kernel has its own unit-test framework, KUnit, which runs test
cases *inside* the kernel (under UML or a VM) and can reach static
functions directly. It is the right tool for testing kernel-side logic
that cannot be lifted into user space. Here, the logic that matters
*could* be lifted — the head protocol and ring arithmetic live in
`include/daqring_ring.h`, used by the test client and the tests alike —
so the user-space tests cover it at a fraction of the setup. If the
driver grew logic that could not be shared that way, KUnit is where it
would be tested.

---

## 5. Gating: how a failed check becomes a rejected change

CI does not merge, and CI does not reject. CI **reports**, and branch
protection **enforces**. They are set separately.

```text
   ┌────────────────────────────────────────────────────────────┐
   │ GitHub → repo → Settings → Branches → Add rule for master  │
   │                                                            │
   │  [x] Require a pull request before merging                 │
   │  [x] Require status checks to pass before merging          │
   │        required: lint, unit tests, module (x86-64),        │
   │                  module (arm64)                            │
   │  [x] Require branches to be up to date before merging      │
   │  [ ] Include administrators   ← on for teams, off solo     │
   └────────────────────────────────────────────────────────────┘
```

With that rule in place the sequence is:

```text
   open PR ──► checks run ──► any red? ──► merge button disabled
                                 │            author fixes, pushes,
                                 │            checks re-run
                                 │
                            all green ──► merge button enabled
                                              │
                            person clicks (or auto-merge fires)
                                              ▼
                                         master updated
```

For a one-person repository the honest configuration is: rule on, "include
administrators" **off**, so the owner can still push directly in an
emergency — while every pull request, including the owner's own, is
gated. Turn "include administrators" on the day a second person joins.

---

## 6. The unit tests, and the bug they found

`test/unit_ring.c` — 25 checks, TAP output, non-zero exit on failure:

```text
   ABI layout          sample is 24 bytes; every field at its documented
                       offset; hdr.head 8-byte aligned; header fits a page
   ring arithmetic     slot wraps at capacity; computed in 64 bits;
                       lap detection at exactly capacity+1; loss counted
   head protocol       a producer thread publishes; a reader thread loads;
                       the counter starts 500 below 2^32 and wraps
                       thousands of times; the reader must never see the
                       value go backwards, never see a value the producer
                       had not written
```

Writing the last test exposed a real defect in the previous 32-bit
protocol. It published high word then low word and had readers retry
if the high word moved between two reads. That misses one interleaving:

```text
   producer                      reader (32-bit)
   ────────                      ───────────────
                                 read hi  = 0
   write hi = 1
                                 read lo  = 0xFFFFFFFF   (old)
                                 read hi2 = 1            (≠ hi → retry, good)

   BUT:                          read hi  = 1            (new)
   (lo not yet written)          read lo  = 0xFFFFFFFF   (old)
                                 read hi2 = 1            (= hi → ACCEPT)
                                 value    = 0x1FFFFFFFF  ← never existed
   write lo = 0
                                 next read = 0x100000000 ← SMALLER than last
```

A consumer that sees head move backwards computes `head − tail` as a
huge unsigned number, concludes it has been lapped, and resyncs to
garbage. Once per 4.3 billion samples — about a day at 50 kHz — and
invisible to every measurement in this repository, because none ran
that long. **A unit test that starts the counter at 2³² − 500 finds it
in milliseconds.**

The fix is a seqlock: the producer brackets each update with a counter
that is odd during the write and even after; a 32-bit reader retries
until it sees the same even value on both sides of its two loads. It
costs the producer two extra 32-bit stores per sample, adds one field
to the shared header (backward compatible — old readers ignore it), and
also makes a 32-bit *reader* safe on a 64-bit *kernel*, a configuration
Raspberry Pi OS actually ships. 64-bit readers still take the native
single-load path. The protocol lives in `include/daqring_ring.h`, used
by the test client and the tests alike, and mirrored in the driver.

---

## 7. Python, C, or C++ for GitHub Actions?

None of them is "the language of GitHub Actions". A workflow is YAML
that names steps; each step runs a shell command. What language you
write depends on what the step *does*:

```text
   layer                       language          in this repository
   ─────────────────────────   ───────────────   ─────────────────────────
   workflow definition         YAML              .github/workflows/*.yml
   steps                       shell (bash)      inline `run:` blocks
   the thing under test        C                 daqring.c, test client
   unit tests                  C                 test/unit_ring.c
   build orchestration         POSIX sh + make   scripts/*.sh, Makefile
   result parsing              awk / grep        inline
```

C for the tests is not a stylistic choice: the tests exercise a memory
protocol with `__atomic` builtins and `pthread` races. That only means
something in the language the driver is written in. Python would be
the right tool for a different job — turning a hundred characterisation
runs into a plot, say — and the moment that job exists, a Python step
goes in beside the others. Kontur asks for "Python for tooling and
test"; that is where it belongs, and not before.

---

## 8. Jenkins, or something better?

```text
                 hosted GitHub Actions   self-hosted runner    Jenkins
                 ─────────────────────   ──────────────────    ───────────────
   server to run          none           the bench machine     a server + JVM
   maintenance            none           OS updates            plugins, upgrades,
                                                               security patches
   cost, public repo      free           electricity           a machine + time
   hardware access        no             yes                   yes
   suits                  everything     hardware-in-the-loop  air-gapped sites,
                          that needs                           mandated on-prem,
                          no hardware                          existing estates
```

**For this repository:** hosted Actions plus a Pi as a self-hosted
runner covers everything, at zero cost, with nothing to maintain.

**For an instrument company:** the same shape scales. GitLab CI or
GitHub Actions with self-hosted runners on the bench is the modern
default; Jenkins is what you inherit, not what you choose in 2026,
unless the site is air-gapped or policy forbids hosted CI. If Kontur
runs Jenkins, the skills transfer directly — a Jenkinsfile is the same
pipeline in a different syntax — but the *design* in §2 is the thing
worth carrying between them.

---

## 9. Reproducibility, the part that actually matters

The image job runs the same `scripts/buildroot-overnight.sh` a person
runs on a laptop. There is no separate "CI way" to build it. That is
deliberate: a build that only works in CI, or only works locally, is a
build nobody trusts.

What this project learned the hard way is that **host drift breaks
builds**: a Raspbian too old to host Buildroot on one side, a GCC too
new for Buildroot's bundled m4 on the other. The build script now pins
what it can (an older host compiler when the default is GCC ≥ 15). The
next step, if this were a product, is a container image with a pinned
base distribution so the host cannot drift at all — the same
Dockerfile used by CI and by every engineer.

---

## 10. Running it

```sh
make unit                      # the unit tests, locally, in seconds
make check                     # alias

# on GitHub:
#   push or open a PR         → ci.yml runs by itself
#   Actions → image → Run     → builds the image, uploads sdcard.img
#   git tag v3.1 && git push --tags
#                             → image built and attached to a Release
#   Actions → hardware-in-the-loop → Run
#                             → the Pi runs the sweep in hardware mode
```
