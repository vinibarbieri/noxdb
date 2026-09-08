# NoxDB — Performance

Measurement record for the NoxDB user-space asynchronous I/O storage engine.

**Status: incomplete.** This document currently contains the *device ceiling*
and the engine numbers measured during cycles C1–C4. The comparative
experiments (E1–E3) and their baselines are not in it yet. Sections marked
🚧 are open. Last measurement run: **2026-08-26**.

Predictions are recorded here *before* the measurement that settles them, and
when one is wrong it is kept, marked wrong, and the reason given (§4.6 is the
current example). A prediction quietly deleted after the fact teaches nothing.

Everything here is measured on the bench box described under
[Environment](#environment). Nothing in this document is extrapolated, and
where a number is known to be inflated it says so next to the number rather
than in a footnote.

---

## 1. Methodology

These five rules gate every benchmark in this repository
(`.dev/KANBAN.md` → C10E-METHOD). Each one has invalidated a published
benchmark by serious people at some point, which is why none is optional.

### M1 · Durability-contract parity 🚧

A buffered `write()` returns when the bytes reach the page cache. An
`O_DIRECT` write returns when they reach the device. Comparing the two
without equalizing the semantics compares a lie to the truth.

**Regime chosen for this document:** every number in §3 is `O_DIRECT`, so no
parity question arises yet — they are device measurements, not a comparison.
When the buffered baseline lands in §4, it will be reported as **two separate
curves with the contract stated on each**: raw buffered (returns at the page
cache) and buffered + `fsync` at a stated cadence. They will not be averaged
together, and neither will be presented as "the" baseline.

### M2 · SLC cache

The WD SN530 is TLC with a dynamic pSLC cache. Sustained writes fall off a
cliff once that cache saturates, so a short benchmark measures the cache and
not the device.

**Applied:** before the §3 sweep the drive was filled to 80% of free space and
overwritten twice — 2 × 374 GiB = 748 GiB total — using 1 MiB `O_DIRECT`
sequential writes. The two passes agreed to within 0.2% (461.8 and
462.7 MB/s), which is itself the evidence that the pSLC cache was saturated
and the drive was in its sustained TLC regime for the second pass.

The filesystem is mounted **without** `discard` and `fstrim.timer` last ran
2026-08-17, so deleting the preconditioning file does **not** return the NAND
to a clean state. Preconditioning therefore stays valid across runs until the
next scheduled TRIM. Check this before reusing a preconditioned drive:

```
findmnt -no OPTIONS /mnt/nvme          # must not contain 'discard'
systemctl list-timers fstrim.timer     # LAST must precede your preconditioning
```

**The protocol was accidentally put on trial, and it held.** On 2026-08-25
19:39 `fstrim.timer` fired unattended and discarded 460.6 GiB on `/mnt/nvme`,
destroying exactly the state described above. The drive was re-preconditioned
by the same recipe and the §3.1 sweep re-run against a pre-declared anchor:

| | before TRIM | after TRIM + re-precondition | Δ |
|---|---:|---:|---:|
| precondition pass 1 | 461.8 MB/s | 460.4 MB/s | −0.3% |
| precondition pass 2 | 462.7 MB/s | 463.3 MB/s | +0.1% |
| **4K QD2 anchor** | **410.0 MB/s** | **408.5 MB/s** | **−0.4%** |

So M2 is not merely a precaution taken once — it is **reproducible across a
full erase of the conditioned state**, which is the stronger claim and the one
that lets §3 survive an accident. `fstrim.timer` is now masked on the bench
box; unmask it when the campaign ends.

### M3 · Run hygiene

`tools/bench_hygiene.sh` applies and verifies:

| Item | Why it matters here |
|---|---|
| `swapoff -a` | Swap lives on the **USB-attached OS disk** on this box. One page-out during a run and the number describes the USB bus. |
| governor `performance` | `ondemand` ramps the clock *during* the measurement, so p99 inherits a frequency transition unrelated to the engine. |
| `drop_caches` before every point | Otherwise run *N* reads what run *N−1* left in the page cache. Distorts the buffered baseline most — the baseline E2 rests on. |
| `fallocate` on test files | Writing into a sparse file mixes XFS extent allocation into the write path. |
| thread pinning | **NOT handled by the script** — it must happen in the process (`taskset`, or `pthread_setaffinity_np`). Not yet applied to the §3 sweep. |

### M4 · Repetition

Minimum 5 repetitions per point; median and spread reported, never a single
number. Spread is reported as `(max − min) / median`.

### M5 · Hardware ceiling first

`fio` before anything else, sweeping queue depth on both shapes the engine has
a path for: **4 KiB random** (the scrap path's workload) and **1 MiB
sequential** (the fast path's). Without the ceiling there is no way to
interpret any NoxDB number — "1.8 GB/s" means nothing until you know what the
device can do.

### M6 · Execution order must not correlate with the variable

*Added 2026-08-20, after it bit us.* See
[Limitations](#5-limitations-and-known-artifacts) → *Recovery transient*.
Repetitions are the outer loop and point order is reshuffled every pass
(`tools/fio_sweep.sh`), so residual drift appears as spread within a point
rather than as slope across the curve.

---

## 2. Environment

```
date          2026-08-20
host          noxdb (bare metal, no hypervisor, no container)
os            Ubuntu Server 24.04 LTS
kernel        6.8.0-100-generic
cpu           AMD Ryzen 7 5700X, 8 cores / 16 threads
memory        15.5 GiB usable (1 × 16 GB, SINGLE CHANNEL since 2026-08-17)
swap          0 kB (enforced; swap device is the USB OS disk)
governor      performance
device        /dev/nvme0n1 — WDC PC SN530 SDBPNPZ-512G-1114 (512 GB, DRAM-less client TLC)
scheduler     none
nr_requests   1023
write_cache   write back
filesystem    XFS on /dev/nvme0n1p1
mount         rw,noatime,attr2,inode64,logbufs=8,logbsize=32k,noquota
mountpoint    /mnt/nvme   (OS disk is a SEPARATE Kingston NV2 over USB — never measured)
```

> **Memory caveat.** This box went to 1 × 16 GB (single channel) on
> **2026-08-17**. Any comparison against a number recorded before that date is
> invalid. The C1 and C3 results in §4 predate it.

Raw provenance for each run is captured automatically at
`/mnt/nvme/results/<timestamp>/provenance.md`
(`tools/bench_hygiene.sh record`).

---

## 3. Device ceiling (M5)

Run `20260820-032106`. Preconditioned per M2. 5 repetitions × 120 s per point,
10 s ramp, `drop_caches` between points, `libaio`, `--direct=1`, 16 GiB working
set.

### 3.1 4 KiB random write — the scrap path's shape

| QD | MB/s (median) | IOPS (median) | p99 (µs) | spread |
|---:|---:|---:|---:|---:|
| 1 | **252.6** † | 61 700 | 14.5 | 1.2% |
| 2 | **410.0** | 100 097 | 23.2 | 0.6% |
| 4 | 419.7 | 102 462 | 220.2 | 0.9% |
| 8 | 420.5 | 102 668 | 415.7 | 1.0% |
| 16 | 420.0 | 102 528 | 692.2 | 0.8% |
| 32 | 420.3 | 102 618 | 1 122.3 | 0.8% |
| 64 | 421.5 | 102 913 | 1 876.0 | 0.5% |

> † **QD1 comes from a separate re-measurement** (`rerun-qd1`, 2026-08-20
> 13:26–14:10), because its five repetitions in the main run climbed
> monotonically (58.2 → 87.7 → 129.3 → 252.2 → 253.0 MB/s) and were withheld.
> The re-run used shuffled point order and carried **QD2 as an anchor**: QD2
> re-measured at 407.3 MB/s against 410.0 here, **−0.7%**, inside both runs'
> spread. The anchor reproduces, so the QD1 figure is comparable to the rest of
> this table. Its own rep1 was discarded as a warm-up transient (53.2 MB/s
> against 250.3/251.9/253.4/253.2); reps 2–5 give 252.6 MB/s at 1.2% spread.
> See §5.

**The knee is between QD1 and QD2.** QD1 → QD2 is **1.61×**; QD2 → QD64 is
**+2.8%**. Everything past QD2 is queueing, not throughput.

**Re-measured with device-side observation (2026-08-26).** §5 asked for this
sweep to be re-run under `iostat -x`, because §3.2.1 had shown that the
requested `--iodepth` is not what reaches the device. It was, on the
re-preconditioned drive, 5 repetitions, shuffled order:

| QD | MB/s (median) | device `aqu-sz` | `%util` |
|---:|---:|---:|---:|
| 1 | 256.9 | 0.57 | 49.8–61.7 |
| 2 | 411.0 | 1.09 | 65.8 |
| 4 | 413.5 | 1.76 | 74.1 |
| 8 | 411.8 | 3.33 | 74.0 |
| 16 | 412.6 | 7.68 | 71.1 |
| 32 | 408.2 | 19.7 | 66.6 |
| 64 | 415.4 | 49.3 | 61.2 |

**No request splitting here, as expected** — 4 KiB is far below
`max_sectors_kb=128`, and `aqu-sz` tracks the requested depth (roughly QD/1.3)
instead of multiplying it the way §3.2.1 showed for 1 MiB. This is the first
time that expectation has been checked rather than assumed, and the reason to
check was that the same assumption was wrong twice before.

The bandwidth column reproduces the table above (QD1 +1.7%, QD2 +0.2%, knee
1.60× against 1.61×). Four of the seven points nevertheless carried a
`MONOTONIC` warning; see §5, *Residual GC bleeds between points*.

### 3.2 1 MiB sequential write — the fast path's shape

| QD | MB/s (median) | p99 (µs) | spread | device `aqu-sz` |
|---:|---:|---:|---:|---:|
| 1 | **638.5** ‡ | 4 358 | 3.1% | **4.29** |
| 2 | **643.9** | 7 176 | 4.1% | **12.28** |
| 4 | 646.5 | 14 746 | 4.3% |
| 8 | 646.5 | 26 346 | 4.5% |
| 16 | 647.6 | 47 448 | 4.4% |
| 32 | 647.0 | 84 410 | 2.7% |

> ‡ **QD1 comes from run `seq-qd1`** (2026-08-20 14:20–15:01), measured with a
> 900 s per-shape warm-up after two earlier attempts drifted. It converged —
> 3.1% spread, no monotonic trend — and its QD2 anchor re-measured at 645.0
> against 643.9 here, **+0.2%**. Comparable.

### 3.2.1 The x-axis of this table is not what it says

The `aqu-sz` column is the queue depth the **device** reported while the load
ran, and it does not match the `--iodepth` requested. The reason:

```
/sys/block/nvme0n1/queue/max_sectors_kb = 128
```

The block layer splits every 1 MiB request into **8 × 128 KiB** device
requests. So "1 MiB sequential at QD1" is roughly QD4 at the device, and QD2 is
roughly QD12.

This resolves what would otherwise look like two different scaling behaviours.
The 1 MiB curve appears to start already at its plateau — QD1 is 638.5 against
a 647 ceiling, 1.3% away — while the 4 KiB curve climbs 1.61× from QD1 to QD2.
There is no contradiction: **4 KiB requests are below the split threshold and
are never divided, so their QD1 really is QD1; 1 MiB requests at QD1 are
already past the knee before the sweep begins.** Corrected for splitting, both
shapes tell the same story.

`%util` was 96.2–97.0% at every point, confirming the device was saturated and
these numbers are device-limited rather than host-limited.

**This was invisible until the sweep started observing the device while the
load ran** (Gregg, *Systems Performance* ch. 12, active benchmarking). Reading
only fio's output would have left a plausible, wrong reading of the sequential
curve in this document.

### 3.3 Sustained sequential, whole-drive

The preconditioning passes themselves are the most honest sustained number
available, because they write 374 GiB rather than cycling a 16 GiB working set:

| Pass | MB/s | Duration |
|---|---:|---:|
| 1 | 461.8 | 869.6 s |
| 2 | 462.7 | 867.9 s |

### 3.4 What the ceiling says

**The device saturates at QD2–4, not at QD32.** From QD2 onward bandwidth is
flat to within 1% while p99 grows *linearly* with queue depth — the signature
of a saturated device, where added queue depth converts directly into waiting
rather than throughput. This is a DRAM-less client SSD; it is not a datacenter
part and does not behave like one.

Concretely, on the 4 KiB random shape, going from QD2 to QD64 buys **+2.8%**.

**Three sustained-write regimes, all real, all different:**

| Regime | MB/s | What it is |
|---|---:|---|
| Burst (fresh drive, pSLC) | ~1 839 | What a short benchmark measures |
| Sustained, 16 GiB working set | ~647 | Preconditioned, small hot region |
| Sustained, whole drive | ~462 | Preconditioned, 374 GiB written |

The burst figure is **2.84×** the 16 GiB sustained figure and **3.98×** the
whole-drive figure. Any NoxDB number quoted against the burst ceiling is
quoting a ratio against a number the device cannot hold.

---

## 4. Engine results

> **Claim discipline.** These are the numbers the engine has produced so far.
> They were measured on a **fresh, un-preconditioned drive** unless stated
> otherwise, and the two oldest predate the memory change. They are recorded
> here for continuity, **not** as the comparative results — those are §5's
> open items.

### 4.1 Fast path vs raw `O_DIRECT` (C1, 2026-07-13)

| | MB/s |
|---|---:|
| `fio` raw `O_DIRECT`, 4 MiB writes | 1 839 |
| NoxDB fast path | 1 779.8 |
| **Ratio** | **96.8%** |

**The ratio is the claim; the absolute numbers are burst.** Both sides were
measured on a fresh drive over a 256 MiB run — entirely inside the pSLC cache.
§3.4 now quantifies that inflation at 2.84–3.98×. What survives is the
architectural statement: *the fast path is a thin pass-through, costing ~3%
over a raw `O_DIRECT` write.* Both sides were inflated equally, so the ratio
holds; the absolutes do not, and must be re-measured on the preconditioned
drive before appearing in any graph.

### 4.2 Sharded index scaling (C3, 2026-07-28)

Threads writing **disjoint** 256 KiB regions. Predates the memory change.

| Threads | MB/s |
|---:|---:|
| 1 | 942.4 |
| 2 | 1 598.0 |
| 4 | 1 890.0 |
| 8 | 1 767.0 |
| 16 | 1 829.5 |

Scales 1→4, then plateaus at ~1.86 GB/s. On a fresh drive that plateau was
read as "device-limited, not lock-limited". §3 now supports that reading with
a real ceiling — but the sustained ceiling is 462–647 MB/s, so these runs were
squarely in pSLC burst (0.07–0.59 s each). **The scaling *shape* is the valid
claim. The absolute MB/s are not.**

ThreadSanitizer: zero data races at 4/8/16 threads.

> **Scope.** This gate gives each thread an exclusive block of regions, so no
> page base is ever shared. It proves the index does not corrupt under
> concurrency and does not serialize; it does **not** prove `nox_write` is
> thread-safe in general.

### 4.3 OTflush soak (C4-G7, 2026-08-18)

1 200 s, 16 threads, 4 shared 256 KiB bases, disjoint byte stripes within each
base — i.e. genuinely overlapping page contention, the case C3 partitioned
away from.

```
250,072,685 writes in 1200.5 s          nox_close 1.223 s
p50 270 ns · p99 39,801 ns · p99.9 80,121 ns
max 9,772,142,759 ns · count>200us 23,683 (0.00947%)
RSS peak 647,060 kB · plateau drift −102,364 kB · ceiling 56.3%
1. INTEGRITY PASS   2. RSS BOUNDED PASS   3. NO STALL PASS
```

Before the C4-B9 eviction watermark, the same run died of OOM at ~6 s having
reached 10.2 GB. `gate-c4` (8 threads, disjoint bases): PASS, backpressure
never engaged, p99.9 68 911 ns. TSan clean.

**The `max` of 9.77 s is a known unfairness, not an outlier.** It does not
converge with run length — 2.11 s at 3 s, 4.12 s at 120 s, 9.77 s at 1 200 s —
while p50, p99.9 and the blocked fraction (~0.009%) stay flat. The watermark
gate wakes its waiters with `pthread_cond_broadcast` and has no fair queue, so
a thread can lose the race arbitrarily many times. Documented here because it
*will* appear in a latency graph, and an unexplained 9.77 s in a graph is worse
than a stated one. Fix (ticket-based wait list) is scoped to C5-rest.

### 4.4 Write amplification vs write size (C4 stats build)

Header crossover is exactly `NOX_DATAZONE_SIZE / NOX_MAX_ENTRIES` =
262144/64 = **4096 B**.

| Mean write size | Pages sealed by entry exhaustion | Amplification | Watermark | Peak RSS | `nox_close` |
|---|---:|---:|---|---:|---:|
| 256 B | 99.97% | 13.47× | engages | 647 MB | 1.223 s |
| 4 KiB | 0.00% | — | never engages | 30 MB | 0.004 s |

Below the crossover nearly every page is evicted because its 64-entry index
ran out, not because its 256 KiB filled — which is what drives the 13.47×.

### 4.5 Write-ordering under multiple Stage-2 threads (2026-08-20)

`src/noxdb_config.h` argues in prose that `NOX_STAGE2_THREADS > 1` allows two
pages for one base to reach the SSD out of order. `bench/order_repro.c` was
written to turn that argument into a measurement. **It could not reproduce it**
— three configurations, all PASS with zero mismatches:

| Build | Amplifier | Predicted | Measured |
|---|---|---|---|
| 1 Stage-2 thread | Stage-1 stall 20 ms | PASS | PASS |
| 4 Stage-2 threads | Stage-1 stall 20 ms | FAIL | **PASS** |
| 4 Stage-2 threads | Stage-2 jitter 0–5 ms | FAIL | **PASS** |

Reading the code explains why, and it is a real finding rather than a failed
test. `stage1_loop` calls `wb_guard_wait(base)` *before* its `pread` and only
then pushes to Q2, so **while any page for base B is pending writeback,
Stage-1 cannot push the next page for B into Q2 at all**. At most one page per
base is ever in Q2. The Stage-2 thread count is therefore irrelevant to
same-base ordering — the guard is stronger than the invariant its own comment
states.

There is exactly one gap, and it is a specific line: the **full-page forward**
branch in `stage1_loop` calls `wb_guard_enter` and pushes to Q2 **without**
first calling `wb_guard_wait`. A page that is already full when Stage-1 pops it
skips the wait and can join a page of the same base that is still in flight.

The driver never produced that state: it writes 8 B records on a 2 048 B
stride, covering at most 1 024 of 262 144 bytes, so **no page ever became
full**. The PASS is therefore structural, and is *not* evidence that the gap is
harmless — it is evidence that this workload does not reach it.

**Consequence for the roadmap.** The reason to fix the ordering hazard was to
unlock more Stage-2 threads. §3.4 shows the device saturates at QD2–4, so more
Stage-2 threads are worth ~2.8%, not a multiple. The fix stays on the list as
correctness hygiene — one `wb_guard_wait` call — and comes off the critical
path.

### 4.6 Effective queue depth and CPU cost of the engine (2026-08-26)

**Prediction, recorded in §5 before this measurement: `aqu-sz` ≥ 4, and 8–16
whenever the `pwritev` batch fires.**

Measured — 300 s soak, 16 threads over 4 shared bases, `iostat -x` on
`/dev/nvme0n1` for the duration:

| | measured |
|---|---:|
| device `aqu-sz` | **1.58** median (1.52 mean, n = 320) |
| device `wareq-sz` | 121.2 KiB |
| device throughput | 490.8 MB/s |
| `%util` | 70.9 |

**The prediction was wrong.** Not marginally — it named a floor of 4 and the
device reported 1.58.

The error was confusing *instantaneous split width* with *time-averaged queue
occupancy*. The arithmetic behind the prediction was right and is confirmed
here: `wareq-sz` came out at 121.2 KiB, so the 2 MiB `pwritev` really is being
split into ~128 KiB device requests exactly as §3.2.1 describes. But `pwritev`
is **synchronous**. One Stage-2 thread enqueues those 16 fragments, blocks
until they drain, and then leaves the queue *empty* while it does Stage-1 work,
takes locks, and copies bytes. `aqu-sz` averages over wall-clock time, so what
it reports is the duty cycle, not the peak. A burst of 16 that occupies a tenth
of the interval reads as 1.6, and that is essentially what happened.

§5 had pre-registered the falsification condition — *"if it comes out near 2,
the prediction was wrong and the ordering fix regains some value"* — and it
fired. The §4.5 conclusion that the ordering fix is off the critical path was
reached under the assumption that the engine sat comfortably past the knee;
that assumption is now measured to be false and §4.5 should be re-read with
that in mind.

**The practical conclusion survives, for a different reason.** §3.1 puts the
bandwidth plateau at QD2 (`aqu-sz` 1.09 → 411 MB/s) and flat to QD64. At 1.58
the engine is already inside that plateau, so **raising `NOX_STAGE2_THREADS`
still buys nothing** — not because the engine floods the queue, but because
the device stops rewarding depth almost immediately. The right conclusion was
reached earlier by the wrong argument.

**Where the cost actually is.** `perf stat` over the same load:

```
      919 559 720 316   cycles
      628 767 678 930   instructions      #  0.68 insn per cycle
       12 678 752       context-switches  #  ~42k/s
           39 753 117   page-faults       #  ~132k/s
        58.4 s user  /  179.3 s sys       over 302.9 s wall
```

Three times as much kernel time as user time, and 0.78 page-faults **per
foreground write** (39.8M faults against 51.3M writes). The engine is not
compute-bound; it is bound by syscalls, scheduling and faulting. That ratio is
the strongest lead this run produced. **§4.7 settles it.**

**Write amplification, device-side.** 51 261 771 writes of 1–512 B (mean
≈ 256 B) is ≈ 13.2 GB of logical data; the device wrote 490.8 MB/s × 300.3 s
= 147.4 GB. That is **≈ 11×**, which corroborates the 13.47× that §4.4 derived
by a completely different route (engine counters, not `iostat`).

> **The 490.8 MB/s is not comparable to §3.3.** `c4soak.dat` is 1 MiB — four
> 256 KiB bases rewritten in a loop for five minutes. A working set that small
> is absorbed by the controller's cache no matter how the drive was
> preconditioned. The `aqu-sz` result does not depend on the region size; the
> throughput figure does, and must not be read as an engine bandwidth number.

**This run does not close C4-G7.** The soak binary says so itself: 300 s is a
smoke run and the G7 criterion requires ≥ 1200 s. All three criteria passed
(integrity, RSS bounded, no stall at p99.9), which validates the driver, not
the gate. The C4-B9 watermark was confirmed armed and binding — throttling at
4096 live pages, RSS plateauing at 481 MB against a 1144 MB ceiling.

**The tail is still there.** `p99.9 ≈ 102 µs` passes the criterion, but the
exact maximum was **8.37 s**, with 4 984 foreground writes above 200 µs. The
off-CPU analysis of that stall remains open (§5); it was 9.77 s when last seen.

### 4.7 Where the page faults come from (2026-09-02)

§4.6 left 39.75M minor faults unexplained. They are the scrap page lifecycle,
and the accusation can be made on arithmetic alone before any code is read:

```
147.4 GB written / 262144 B per page   =  562 238 page cycles
562 238 cycles x 64 pages of 4 KiB     =   36.0 M faults
measured                               =   39.8 M faults      (ratio 1.10)
```

A 10% agreement accuses `scrap_page_alloc`/`scrap_page_free`; it does not
convict them, because the engine does many other things at once. `bench/fault_probe.c`
removes the engine from the question — it performs only the allocation pattern,
with no noxdb headers, no `O_DIRECT` and no device, and counts `ru_minflt`:

| arm | faults / cycle | 20 000 cycles |
|---|---:|---:|
| A · `posix_memalign` + `free` each cycle (today) | **65.00** | 2.098 s |
| B · allocate once, reuse | 0.00 | 0.004 s |
| C · A, with `mallopt` tuned | 0.00 | 0.005 s |

65 against the 64 predicted; the extra one is the header `malloc`. Identical
across two runs. **The touch loop is the same in all three arms**, so the
2.094 s separating A from B is allocation and fault handling, not memory
traffic — 1.61 µs per fault.

**Two glibc policies, not one.** Arm C sets both `M_MMAP_THRESHOLD` and
`M_TRIM_THRESHOLD`, so it does not say which one mattered. Re-running arm A in
fresh processes under the environment variables separates them:

| glibc configuration | faults / cycle |
|---|---:|
| default | 65.00 |
| `MALLOC_TRIM_THRESHOLD_` raised | 65.00 |
| `MALLOC_MMAP_THRESHOLD_` raised | 33.00 |
| **both raised** | **0.00** |

A 256 KiB zone sits above the 128 KiB `mmap` threshold, so every
`posix_memalign` is an `mmap` and every `free` a `munmap` — that is the first
65. Raising the threshold moves the zone onto the heap, where `free` then
*trims* it back to the kernel — that is the remaining 33. Only disabling both
reaches zero. **Fixing either one alone would have looked like a 50% win and
stopped there**, which is the reason this was measured in four configurations
instead of two.

This also completes a story C4 started. The comment in `scrap_page_alloc`
already identified the mechanism — "the 256KB zone is fresh virtual memory, so
the memset was touching all 64 of its 4K pages" — and removed the eager
`memset` to get those faults off the foreground thread. That worked, and §4.6
shows what it did not do: the faults were moved, not removed.

**Not fixed, deliberately.** The remedy is a recycling pool for the data zones
(`scrap_page_alloc` would take a zone from a free list instead of the
allocator), designed and costed at roughly 150 lines plus a test. It is not
being built, because it is a *feature*, and the scope for this term freezes
the engine at measurement: no new engine features, only the measurement
sweep. The finding is the deliverable; the fix is out of scope. The same
decision is why there is no WAL and no recovery (README → *Credit*).

**Prediction, recorded for whoever does build it:** the pool removes ~36M of
the 39.8M faults and returns ~58 s of the 179 s of system time (36.0M ×
1.61 µs). Throughput should barely move, because §4.6 shows the engine is not
waiting on the device. The latency tail should improve, because some of this
fault handling currently lands on the foreground thread.

**Consequence for the thread-scalability sweep.** `munmap` takes the process's
`mmap_lock` for write. At the 1874 page-cycles/s measured here that is probably
not a serialisation point — but "probably" is doing real work in that sentence,
and a thread sweep that is actually measuring `mmap_lock` would look exactly
like an engine that does not scale. Extending `fault_probe.c` to N threads
settles it, and costs no new engine code.

---

## 5. Limitations and known artifacts

**Start-of-load transient, confounded with queue depth (resolved).**
The `20260820-032106` sweep ran nested loops — shape, then QD, then repetition
— so the first point measured was `randwrite4k` QD1. Its five repetitions
climbed monotonically (58.2 → 253.0 MB/s): the entire transient landed inside
one queue depth, systematically penalising the lowest QD. That is the exact
shape of a fake scaling curve.

`tools/fio_sweep.sh` now makes repetition the outer loop and reshuffles point
order each pass (M6). The re-run on the same drive shows what that buys:

| point | rep1 | rep2 | rep3 | rep4 | rep5 |
|---|---:|---:|---:|---:|---:|
| 4K QD1 | **53.2** | 250.3 | 251.9 | 253.4 | 253.2 |
| 4K QD2 | **79.5** | 406.7 | 407.9 | 406.4 | 409.1 |

All four points' `rep1` were the lowest of their group, and those four were the
first four points chronologically. **The cause was never garbage collection**
— the drive had been idle six hours — it is a start-of-load transient, and
fio's `--ramp_time=10` is two orders of magnitude short of covering it. With
shuffling, that no longer hides as a slope across the curve; it shows up as
79.5% spread *inside a point*, which is a warning rather than a result.
`tools/fio_sweep.sh` now also runs a per-shape `WARMUP` job (default 300 s,
discarded) before the first measured point.

**Two shapes, two time constants (open).**
4K random converges within one repetition. 1M sequential had **not** converged
after five: 438.8 → 479.9 → 493.6 → 516.8 → 511.7 MB/s at QD1, still climbing.
The re-run's QD2 anchor for that shape came out at 507.3 against 643.9 in the
main run — **−21.2%, does not reproduce** — because the main run measured this
shape at the *end* of a two-hour sweep, fully conditioned. One `ramp_time`
cannot serve both shapes. The §3.2 QD1 figure remains unmeasured; §3.2 QD2–QD32
stand.

Both artifacts are now detected automatically by `tools/fio_report.py`, which
flags monotonic repetitions and non-reproducing anchors rather than quietly
taking a median of a transient.

**No thread pinning.** M3 requires it and the §3 sweep did not use it.

**Residual GC bleeds between points (open).**
The 2026-08-26 sweep flagged `MONOTONIC` on four of seven points. The clearest
case is 4K QD1, whose rep5 fell to 123.9 MB/s against 255.8–257.7 for reps 1–4
— but with `%util` *rising* to 74.6 and `aqu-sz` *falling* to 0.46. Bandwidth
halved while the device got busier and its queue got emptier: that is the
device doing internal work, not the load changing. Chronology names the likely
source — that point started at 04:51, two minutes after 4K QD64 rep4 had
finished hammering the drive at depth 49.

This is a *different* artifact from the start-of-load transient above, and the
distinction matters: that one was worst in **rep1** and the drive had been idle
six hours, so GC was ruled out. This one is worst in **rep5**, after 80 minutes
of continuous load, and the device-side counters point the other way.

`tools/fio_sweep.sh` applies `SETTLE` **once**, before the first measured
point, and only `RAMP=10` between runs. Ten seconds does not drain a GC
backlog. This is a fresh gap in M6: point order is shuffled, but the *previous*
point's after-effects still ride into the next one, so a heavy point
systematically penalises whatever follows it. **Fix: a per-point settle, and
record each point's predecessor so the contamination is auditable.** The four
affected points should then be re-measured. Medians and the QD2 anchor were
unaffected (§3.1), so §3.1's values stand; the spreads do not.

**§3.3 has no device-side confirmation.** `sysstat` was installed only after
the earliest sweeps ran. §3.1 has since been re-measured under `iostat -x`
(2026-08-26) and §3.2 QD1/QD2 always carried it, but the whole-drive sustained
run has not been repeated.

**Single device, single filesystem.** One SN530 on XFS. Nothing here
generalises to another SSD class — least of all to a DRAM-equipped datacenter
drive, whose queue-depth curve would look nothing like §3.1.

**`noxdb commit` is not captured.** `tools/bench_hygiene.sh record` runs on the
bench box, which receives only source files by `rsync` and is not a git
checkout, so the commit field records `?`. Must be filled from the dev machine.

**Results live on the device under test.** `/mnt/nvme/results/` shares the
drive being measured. The JSON writes are tiny and happen between timed runs,
but it is not clean and should move off-device.

### Open — not yet measured 🚧

- **E1/E2/E3 and their baselines.** No buffered-`write()` baseline exists yet,
  so nothing in this document is a comparison against anything.
- **The `i_rwsem`-vs-XArray question.** A buffered write takes the inode's
  `i_rwsem` exclusively; a non-extending `O_DIRECT` write takes it shared. If
  E2 uses a single file, a baseline plateau may be caused by `i_rwsem` and not
  by the XArray. **This must be settled with a profiler** — looking for
  `rwsem_down_write_slowpath` versus the radix-tree paths — before any claim is
  made about *why* the baseline plateaus.
- **CPU efficiency curves.** First point collected (§4.6): IPC 0.68, sys:user
  3:1, ~132k page-faults/s. Not yet a *curve* — one load, one thread count.
  The page-fault rate now has a measured cause (§4.7); the remaining sys time
  does not.
- **Thread-scalability sweep.** The §3 sweeps vary *queue depth*, which is a
  property of the device. Varying noxdb's own thread count is a different
  measurement and has not been made. See §4.7 for a confound to rule out first.
- **Effective queue depth of the engine.** ✅ **Measured 2026-08-26 — see
  §4.6. The prediction recorded here was wrong: predicted ≥ 4, measured 1.58.**
- **The 8.37 s foreground stall.** §4.6 records the exact maximum; the cause is
  not established. Needs off-CPU analysis, not another soak.
- **C4-G7 at full length.** §4.6's soak was a 300 s smoke run; the criterion
  requires ≥ 1200 s.

### Not claimable

> ~~"I eliminated the read-before-write penalty."~~

What exists today is a characterization of the device and of the engine's own
behaviour. The comparative claim requires §5's open items. What *is* claimable
is narrower and still worth stating: *the problem was measured, here is the
device's real ceiling, and here is what the engine does against it.*

---

## Reproducing

```sh
# on the dev machine
make deploy

# on the bench box, as root
./tools/bench_hygiene.sh check          # swap 0, governor, correct device
./tools/bench_hygiene.sh apply

# full night: hygiene -> ordering repro -> precondition -> sweep -> provenance
./tools/overnight.sh

# unattended, survives logout: mask fstrim -> hygiene -> precondition ->
# 4K sweep -> engine soak. Builds the soak binary itself and REFUSES to run it
# if the watermark is compiled out, because that build OOM-kills the box.
sudo systemd-run --unit=noxdb-today --collect \
     --working-directory=$PWD --setenv=SKIP_PRECOND=1 ./tools/today.sh

# sweep only, e.g. re-measuring QD1 with QD2 as a comparability anchor
./tools/fio_sweep.sh /mnt/nvme/results/rerun "1 2"

# read the result from anywhere; the anchor is the check that matters
./tools/fio_report.py /mnt/nvme/results/<run>/rw4k-active \
     --anchor=randwrite4k:2:410.0

# §4.7 page-fault probe. Pure RAM, no device, no O_DIRECT -- runs anywhere
# glibc runs, and needs no hygiene because it touches no disk.
cc -std=c11 -O2 -o bench/fault_probe bench/fault_probe.c && ./bench/fault_probe
MALLOC_MMAP_THRESHOLD_=1048576 MALLOC_TRIM_THRESHOLD_=16777216 ./bench/fault_probe

./tools/bench_hygiene.sh restore        # give the box back its swap + governor
```
