#!/usr/bin/env bash
#
# baseline_sweep.sh - the page-cache baseline NoxDB does not yet have.
#
# WHAT THIS MEASURES, AND WHAT IT DOES NOT
# ----------------------------------------
# PERFORMANCE.md section 5 says it plainly: no baseline exists, so nothing in
# that document is a comparison against anything. This script produces the
# missing half -- but it is the PROBLEM, not the SOLUTION. It measures what the
# page cache costs on this device at this concurrency. It does NOT measure
# NoxDB against it; that is E2 and needs engine code. Do not let a reader
# conflate the two, and do not write a sentence that lets them.
#
# THE INDEPENDENT VARIABLE IS CONCURRENCY, NOT QUEUE DEPTH
# --------------------------------------------------------
# fio_sweep.sh sweeps --iodepth with libaio: that is a property of the DEVICE
# and section 3 already settled it (the knee is QD2). This sweeps --numjobs
# with psync: N threads each blocking in one write(), which is a property of
# the KERNEL PATH and is also the exact shape NoxDB has -- a blocking pwrite
# per thread. The two sweeps answer different questions and their numbers must
# never be plotted on one axis.
#
# ONE IOENGINE ACROSS ALL CONTRACTS, ON PURPOSE
# ---------------------------------------------
# libaio effectively requires --direct=1; using it for O_DIRECT and psync for
# buffered would vary the engine and the durability contract together, and the
# result would attribute to one what the other caused. psync serves both, so it
# serves both here.
#
# THE THREE CONTRACTS ARE REPORTED SEPARATELY (C10E-METHOD M1)
# ------------------------------------------------------------
#   buffered   write() returns when the bytes reach the page cache
#   bufsync    ...plus fsync every FSYNC_EVERY writes, an explicit cadence
#   direct     write() returns when the bytes reach the device
#
# They are NEVER averaged and neither buffered curve is "the" baseline.
# Comparing a page-cache return against a device return without saying so is
# comparing a lie to the truth, and it is the first thing a reviewer points at.
#
# THE TWO LAYOUTS ARE THE ACTUAL EXPERIMENT
# -----------------------------------------
# A buffered write takes the inode's i_rwsem EXCLUSIVELY; a non-extending
# O_DIRECT write takes it shared. So a buffered plateau across threads has two
# candidate causes -- the page cache's XArray, or i_rwsem -- and the literature
# (and, until this week, this repository's own README) names only the first.
#
#   shared   all jobs write ONE file  -> one inode, i_rwsem contended
#   perjob   each job writes its own  -> i_rwsem is per-inode, so it is REMOVED
#
# If the plateau disappears under `perjob`, it was i_rwsem. If it survives, the
# cache is implicated. Either outcome is a result, and neither may be claimed
# from the throughput curve alone -- see the perf profile this script captures.
#
# WORKING SET IS HELD AT TOTAL_SIZE, NOT AT SIZE-PER-JOB
# ------------------------------------------------------
# The box has 15.5 GiB of RAM. A buffered run whose working set fits in RAM
# measures memcpy, not storage, and would produce a spectacular meaningless
# number. TOTAL_SIZE (default 64G, ~4x RAM) is therefore held CONSTANT across
# every point and divided among the jobs. The cost is that per-job locality
# changes along the sweep; that is a stated limitation, and it is the lesser
# of the two evils.
#
# USAGE
#   sudo ./tools/baseline_sweep.sh /mnt/nvme/results/baseline
#   sudo ./tools/baseline_sweep.sh RESULT_DIR "1 2 4 8 16" "buffered direct"
#
#   SMOKE=1 sudo ./tools/baseline_sweep.sh /mnt/nvme/results/smoke
#       one rep, 20s points, no warm-up, no profile. Proves the script runs end
#       to end BEFORE a night is committed to it. Its numbers are worthless.
#
# Env: REPS(5) RUNTIME(60) RAMP(10) WARMUP(300) TOTAL_SIZE(64G)
#      FSYNC_EVERY(32) ANCHOR_MBPS(410.0) PROFILE_JOBS(16) ACTIVE(1) DEV(auto)
#
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)

RESULT_DIR=${1:?usage: baseline_sweep.sh RESULT_DIR [NUMJOBS] [CONTRACTS] [LAYOUTS]}
NUMJOBS=${2:-"1 2 4 8 16"}
CONTRACTS=${3:-"buffered bufsync direct"}
LAYOUTS=${4:-"shared perjob"}

SMOKE=${SMOKE:-0}
if [ "$SMOKE" = "1" ]; then
    REPS=${REPS:-1}; RUNTIME=${RUNTIME:-20}; RAMP=${RAMP:-5}
    WARMUP=${WARMUP:-0}; TOTAL_SIZE=${TOTAL_SIZE:-8G}; PROFILE_JOBS=${PROFILE_JOBS:-0}
else
    REPS=${REPS:-5}; RUNTIME=${RUNTIME:-60}; RAMP=${RAMP:-10}
    WARMUP=${WARMUP:-300}; TOTAL_SIZE=${TOTAL_SIZE:-64G}; PROFILE_JOBS=${PROFILE_JOBS:-16}
fi

FSYNC_EVERY=${FSYNC_EVERY:-32}
ANCHOR_MBPS=${ANCHOR_MBPS:-410.0}
ACTIVE=${ACTIVE:-1}
BS=${BS:-4k}
RW=${RW:-randwrite}
MNT=${MNT:-/mnt/nvme}

mkdir -p "$RESULT_DIR" || exit 1
LOG="$RESULT_DIR/sweep.log"
DATA_DIR="$MNT/baseline_data"

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

# Device backing the mountpoint, for iostat. Resolved rather than hardcoded so
# the script does not silently observe the wrong disk on a rebuilt box.
DEV=${DEV:-$(findmnt -no SOURCE "$MNT" 2>/dev/null | sed 's:.*/::; s/p[0-9]*$//')}

# ---------------------------------------------------------------------------
# Preflight. Every one of these has a failure mode that produces a NUMBER
# rather than an error, which is the dangerous kind.
# ---------------------------------------------------------------------------
log "=== baseline sweep ==="
log "result dir : $RESULT_DIR"
log "contracts  : $CONTRACTS"
log "layouts    : $LAYOUTS"
log "numjobs    : $NUMJOBS"
log "shape      : $RW $BS via psync, working set $TOTAL_SIZE held constant"
log "reps=$REPS runtime=${RUNTIME}s ramp=${RAMP}s warmup=${WARMUP}s smoke=$SMOKE"
log "device     : ${DEV:-<unresolved>}"

[ "$(id -u)" -eq 0 ] || { log "FATAL: must run as root (drop_caches, perf)"; exit 1; }
command -v fio    >/dev/null || { log "FATAL: fio not installed"; exit 1; }
command -v iostat >/dev/null || log "WARN: sysstat missing -- no device-side view"
command -v perf   >/dev/null || log "WARN: perf missing -- no CPU-side view"

# The whole buffered arm is void if the working set fits in RAM: the cache
# absorbs everything and the curve describes memcpy, not storage.
#
# awk, not bc -- bc is not installed on the bench box and a preflight check that
# dies on its own dependency is worse than no check.
ram_kb=$(awk '/MemTotal/{print $2}' /proc/meminfo)
ram_gib=$(( ram_kb / 1048576 ))
ws_gib=${TOTAL_SIZE%[Gg]}
case "$ws_gib" in
    ''|*[!0-9]*) log "FATAL: TOTAL_SIZE must look like 64G, got '$TOTAL_SIZE'"; exit 1 ;;
esac
log "RAM ${ram_gib} GiB vs working set ${ws_gib} GiB (ratio $(awk -v w="$ws_gib" -v r="$ram_gib" 'BEGIN{printf "%.1f", w/r}'))"

if [ "$ws_gib" -lt $(( ram_gib * 2 )) ]; then
    if [ "$SMOKE" = "1" ]; then
        # SMOKE exists to exercise the plumbing in twenty minutes, and its
        # numbers are declared worthless before it starts. Enforcing a validity
        # floor on a run whose output nobody may quote is the check firing at
        # the wrong target -- so it warns here and still refuses below.
        log "SMOKE: working set is UNDER 2x RAM. The buffered arm will measure"
        log "SMOKE: the cache absorbing everything. Expected, and exactly why"
        log "SMOKE: none of these numbers may be quoted. Continuing."
    else
        log "FATAL: working set under 2x RAM. The buffered curve would measure the"
        log "       page cache absorbing everything, i.e. memcpy, not storage."
        log "       Raise TOTAL_SIZE. Refusing to produce that number."
        exit 1
    fi
fi

# Swap on this box lives on the USB-attached OS disk. One page-out during a
# buffered run and the number describes the USB bus.
if [ "$(awk '/SwapTotal/{print $2}' /proc/meminfo)" != "0" ]; then
    log "FATAL: swap is on. Run ./tools/bench_hygiene.sh apply first."
    exit 1
fi

free_gib=$(df -BG --output=avail "$MNT" 2>/dev/null | tail -1 | tr -dc '0-9')
if [ -n "$free_gib" ] && [ "$free_gib" -lt $(( ws_gib + 8 )) ]; then
    log "FATAL: only ${free_gib} GiB free on $MNT, need ~$(( ws_gib + 8 ))"
    exit 1
fi

mkdir -p "$DATA_DIR" || exit 1

# ---------------------------------------------------------------------------
# FILE GEOMETRY, FIXED FOR THE WHOLE RUN.
#
# The first smoke run failed its own control: the `direct` arm should show
# app/dev == 1.00 by construction, and it came out 0.67-1.11. Two causes, both
# from letting the files vary with the point.
#
#  1. --fallocate=native reserves space but leaves the extents UNWRITTEN. The
#     first write into one makes XFS convert the extent and journal it, on the
#     same device -- writes iostat sees and fio does not report. That alone
#     puts dev above app.
#  2. The job name carried the point tag and the per-job size changed with
#     numjobs, so fio re-laid the files out AT EVERY POINT. direct_shared_nj4
#     showed the consequence undisguised: %util 10.7, the device idle 89% of a
#     20 s window while fio built files.
#
# So: MAXJOBS files of PER_FILE GiB, created and FULLY WRITTEN once, then never
# touched again. A point with nj jobs gives each job MAXJOBS/nj of them --
# every job still owns its files (which is what makes `perjob` a valid i_rwsem
# control), the total working set stays at TOTAL_SIZE for every point, and no
# point pays layout. The job name is fixed per layout for the same reason.
# ---------------------------------------------------------------------------
MAXJOBS=$(printf '%s\n' $NUMJOBS | sort -n | tail -1)
PER_FILE=$(( ws_gib / MAXJOBS ))
[ "$PER_FILE" -lt 1 ] && PER_FILE=1

for nj in $NUMJOBS; do
    if [ $(( MAXJOBS % nj )) -ne 0 ]; then
        log "FATAL: numjobs '$nj' does not divide MAXJOBS ($MAXJOBS)."
        log "       The working set would differ between points and the curve"
        log "       would compare points measured against different amounts of"
        log "       data. Use powers of two."
        exit 1
    fi
done
log "geometry   : $MAXJOBS files x ${PER_FILE}G = $(( MAXJOBS * PER_FILE ))G, laid out once"

# The observers start with fio, so their first RAMP seconds cover fio's own
# discarded ramp. Taking a median over those rows would drag the device number
# down by exactly the amount the ramp exists to exclude. Recorded here so the
# report drops the same rows rather than guessing.
printf 'ramp=%s\nruntime=%s\nreps=%s\ntotal_size=%s\nfsync_every=%s\nbs=%s\nrw=%s\n' \
    "$RAMP" "$RUNTIME" "$REPS" "$TOTAL_SIZE" "$FSYNC_EVERY" "$BS" "$RW" \
    > "$RESULT_DIR/sweep.meta"

# STATED LIMITATION: changing --numjobs under `perjob` changes the per-job file
# size, so fio re-lays the files out at some points. --fallocate=native keeps
# that cheap, but the first write into a fallocated extent still pays unwritten-
# extent conversion in XFS. It applies equally to all three contracts, so it
# does not bias the comparison between them -- it does add a floor to every
# point, and it belongs in the limitations section of any writeup.

# ---------------------------------------------------------------------------
# ANCHOR (M2/M6). Preconditioning was applied on 2026-08-26 and fstrim is
# masked, so it SHOULD still hold -- but "should" is what an anchor exists to
# replace. This reproduces one point from section 3.1 in its ORIGINAL
# configuration (libaio, direct, iodepth 2, 16G file), because a point measured
# a different way is not a check on anything.
#
# If it does not reproduce, the drive is not in the state section 3 was measured
# in and NOTHING from this run may be compared against that document.
# ---------------------------------------------------------------------------
anchor() {
    log "--- anchor: 4k randwrite libaio QD2 direct, expecting ${ANCHOR_MBPS} MB/s ---"
    "$HERE/bench_hygiene.sh" drop >/dev/null 2>&1
    fio --name=anchor --filename="$MNT/anchor.dat" --size=16G \
        --rw=randwrite --bs=4k --direct=1 --ioengine=libaio --iodepth=2 \
        --runtime=60 --time_based --ramp_time=10 --fallocate=native \
        --group_reporting --output-format=json \
        --output="$RESULT_DIR/anchor.json" >>"$LOG" 2>&1

    local got
    got=$(python3 -c "import json;print('%.1f'%(json.load(open('$RESULT_DIR/anchor.json'))['jobs'][0]['write']['bw_bytes']/1e6))" 2>/dev/null)
    [ -z "$got" ] && { log "ANCHOR: unreadable result"; return 1; }

    local delta
    delta=$(python3 -c "print('%+.1f'%(($got-$ANCHOR_MBPS)/$ANCHOR_MBPS*100))")
    log "ANCHOR: ${got} MB/s vs ${ANCHOR_MBPS} expected  (${delta}%)"
    python3 -c "import sys; sys.exit(0 if abs($got-$ANCHOR_MBPS)/$ANCHOR_MBPS < 0.05 else 1)"
}

if [ "$SMOKE" != "1" ]; then
    if anchor; then
        log "ANCHOR OK -- this run is comparable to PERFORMANCE.md section 3."
    else
        log "!!! ANCHOR FAILED (>5% off). The drive is not in the conditioned"
        log "!!! state. Re-precondition, or treat this run as self-contained"
        log "!!! and comparable to NOTHING in PERFORMANCE.md. Continuing, but"
        log "!!! this warning belongs next to every number that follows."
    fi
fi

# ---------------------------------------------------------------------------
# fio invocation for one point. The three contracts differ in exactly two
# flags, which is the point -- everything else is held identical.
# ---------------------------------------------------------------------------
# Emit an explicit fio job file.
#
# WHY A JOB FILE INSTEAD OF FLAGS: fio derives each file's name from the job
# name AND the job index, so `--nrfiles` changing with numjobs renames every
# file and forces a fresh layout at every point -- which is half of why the
# first smoke run's control failed. Naming the files here decouples them from
# the point entirely: one fixed pool, laid out once, reused by every point.
write_jobfile() {
    local contract=$1 layout=$2 nj=$3 jf=$4
    local direct=0 fsync=0

    case "$contract" in
        buffered) direct=0; fsync=0 ;;
        bufsync)  direct=0; fsync=$FSYNC_EVERY ;;
        direct)   direct=1; fsync=0 ;;
    esac

    cat > "$jf" <<EOF
[global]
rw=$RW
bs=$BS
ioengine=psync
iodepth=1
direct=$direct
fsync=$fsync
fallocate=none
randrepeat=0
norandommap
time_based=1
runtime=$RUNTIME
ramp_time=$RAMP
group_reporting=1
EOF

    if [ "$layout" = "shared" ]; then
        # Every job writes the SAME inode, so i_rwsem is contended.
        cat >> "$jf" <<EOF

[w]
numjobs=$nj
filename=$DATA_DIR/shared.dat
size=$TOTAL_SIZE
EOF
    else
        # Each job owns a DISJOINT slice of the fixed file pool. No inode is
        # ever shared, so i_rwsem cannot be the contended lock -- which is the
        # entire point of this layout. Every job count uses all MAXJOBS files,
        # so the working set is identical across the sweep.
        local per=$(( MAXJOBS / nj )) j i first
        for j in $(seq 0 $(( nj - 1 ))); do
            first=$(( j * per ))
            local list=""
            for i in $(seq "$first" $(( first + per - 1 ))); do
                list="${list}${list:+:}$(printf '%s/pj%02d' "$DATA_DIR" "$i")"
            done
            printf '\n[w%d]\nnumjobs=1\nfilename=%s\nfilesize=%sG\n' \
                   "$j" "$list" "$PER_FILE" >> "$jf"
        done
    fi
}

run_fio() {
    local contract=$1 layout=$2 nj=$3 out=$4
    local jf="${out%.json}.fio"
    write_jobfile "$contract" "$layout" "$nj" "$jf"
    fio "$jf" --output-format=json --output="$out" >>"$LOG" 2>&1
}

# ---------------------------------------------------------------------------
# ACTIVE BENCHMARKING (Gregg, Systems Performance ch. 12).
#
# For the DIRECT arm, fio's number and the device's number agree by
# construction. For the BUFFERED arm they do not, and THAT DIVERGENCE IS THE
# RESULT: fio reports the rate at which the application filled the page cache,
# which is a memcpy rate, while iostat reports what the NAND actually received.
# A buffered curve read from fio alone is not a storage measurement at all, and
# reporting one would be the single most embarrassing error available here.
#
# Three observers, each answering a question the fio JSON cannot:
#
#   iostat -x   what the DEVICE did: MB/s, aqu-sz, wareq-sz, %util.
#   /proc/meminfo  Dirty and Writeback, sampled per second. Shows the cache
#               filling and whether writeback throttling engaged -- the
#               mechanism that makes a buffered curve bend downward.
#   perf stat   where the CPU went: sys vs user, context switches, faults.
#               Section 4.6 measured the engine at 3:1 sys:user; this is the
#               same measurement for the kernel path it is being compared to.
# ---------------------------------------------------------------------------
start_observers() {
    local tag=$1 secs=$2
    OBS_PIDS=()

    if [ "$ACTIVE" != "1" ]; then return; fi

    if [ -n "$DEV" ] && command -v iostat >/dev/null; then
        iostat -x -y 1 "$secs" "/dev/$DEV" > "$RESULT_DIR/${tag}.iostat" 2>/dev/null &
        OBS_PIDS+=($!)
    fi

    # Dirty/Writeback sampler. Trivially cheap and it is the only window into
    # the buffered arm's actual mechanism.
    ( for _ in $(seq 1 "$secs"); do
        awk -v t="$(date +%s)" '/^Dirty:/{d=$2} /^Writeback:/{w=$2}
                                END{printf "%s %d %d\n", t, d, w}' /proc/meminfo
        sleep 1
      done ) > "$RESULT_DIR/${tag}.dirty" 2>/dev/null &
    OBS_PIDS+=($!)

    if command -v perf >/dev/null; then
        perf stat -a -x, -o "$RESULT_DIR/${tag}.perfstat" \
             -e context-switches,page-faults,cycles,instructions \
             -- sleep "$secs" >/dev/null 2>&1 &
        OBS_PIDS+=($!)
    fi
}

stop_observers() {
    for p in "${OBS_PIDS[@]:-}"; do wait "$p" 2>/dev/null; done
}

# One line per point, printed as it finishes, so the run can be followed with
# `tail -f` instead of read at the end. app vs dev is the headline: on the
# buffered arm a ratio far above 1 means the page cache is absorbing the load.
summarise() {
    local tag=$1
    python3 - "$RESULT_DIR" "$tag" "$RAMP" <<'PY' 2>/dev/null || echo "  (summary unavailable)"
import json, os, sys, statistics
d, tag = sys.argv[1], sys.argv[2]
skip = int(sys.argv[3])   # drop the rows covering fio's discarded ramp

def med(v): return statistics.median(v) if v else 0.0

app = p99 = 0.0
try:
    j = json.load(open(os.path.join(d, tag + ".json")))["jobs"][0]["write"]
    app = j["bw_bytes"] / 1e6
    p99 = j["clat_ns"]["percentile"].get("99.000000", 0) / 1000.0
except Exception:
    pass

dev = aqu = util = wareq = 0.0
try:
    rows, hdr = [], None
    for line in open(os.path.join(d, tag + ".iostat")):
        f = line.split()
        if not f: continue
        if f[0] == "Device":
            hdr = {n: i for i, n in enumerate(f)}
        elif hdr and len(f) > 3:
            rows.append(f)
    rows = rows[skip:] or rows
    if hdr and rows:
        def col(n): return [float(r[hdr[n]]) for r in rows if n in hdr and len(r) > hdr[n]]
        # wMB/s is the device's own throughput -- the honest one.
        for cand in ("wMB/s", "wkB/s"):
            if cand in hdr:
                v = col(cand)
                dev = med(v) / (1024.0 if cand == "wkB/s" else 1.0)
                break
        aqu, util = med(col("aqu-sz")), med(col("%util"))
        if "wareq-sz" in hdr: wareq = med(col("wareq-sz"))
except Exception:
    pass

dirty_peak = 0
try:
    dirty_peak = max(int(l.split()[1]) for l in open(os.path.join(d, tag + ".dirty")) if l.split()) // 1024
except Exception:
    pass

ratio = (app / dev) if dev > 0 else 0.0
flag = "  <-- CACHE ABSORBING" if ratio > 1.5 else ""
print("  app=%8.1f MB/s  dev=%7.1f MB/s  app/dev=%5.2fx  p99=%9.1fus  "
      "dirty_peak=%5dMB  aqu=%5.2f  util=%5.1f  wareq=%6.1fKB%s"
      % (app, dev, ratio, p99, dirty_peak, aqu, util, wareq, flag))
PY
}

# ---------------------------------------------------------------------------
# Warm-up. The 2026-08-20 run showed a start-of-load transient two orders of
# magnitude longer than fio's --ramp_time, and it landed entirely inside the
# first point measured. One discarded warm-up per contract, before anything is
# recorded.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# LAYOUT PASS. Runs once, and every measured point depends on it.
#
# fallocate alone reserves space and leaves the extents UNWRITTEN, so the first
# write into each one makes XFS convert it and journal the conversion -- device
# writes that fio never reports and iostat always sees. In the first smoke run
# that pushed the direct arm's app/dev to 0.67 when it must be 1.00.
#
# Writing every byte once removes it: from then on every measured write lands
# on an already-written extent, which is also what a steady-state workload
# actually does. Sequential 1M O_DIRECT because the goal is to fill the files
# quickly, not to measure anything.
# ---------------------------------------------------------------------------
layout_pass() {
    local need=0 i
    [ -s "$DATA_DIR/shared.dat" ] || need=1
    for i in $(seq 0 $(( MAXJOBS - 1 ))); do
        [ -s "$(printf '%s/pj%02d' "$DATA_DIR" "$i")" ] || need=1
    done
    if [ "$need" = "0" ]; then
        log "layout: file pool already present, skipping (rm -rf $DATA_DIR to rebuild)"
        return
    fi

    log "layout: writing $(( ws_gib * 2 ))G once -- shared.dat plus $MAXJOBS x ${PER_FILE}G"
    log "        (this is what makes the direct arm's app/dev == 1.00 control valid)"
    fio --name=layout_shared --filename="$DATA_DIR/shared.dat" --size="$TOTAL_SIZE" \
        --rw=write --bs=1M --direct=1 --ioengine=psync --numjobs=1 \
        --fallocate=none >>"$LOG" 2>&1

    local list=""
    for i in $(seq 0 $(( MAXJOBS - 1 ))); do
        list="${list}${list:+:}$(printf '%s/pj%02d' "$DATA_DIR" "$i")"
    done
    fio --name=layout_pool --filename="$list" --filesize="${PER_FILE}G" \
        --rw=write --bs=1M --direct=1 --ioengine=psync --numjobs=1 \
        --fallocate=none >>"$LOG" 2>&1
    log "layout: done"
}
layout_pass

if [ "$WARMUP" -gt 0 ]; then
    for c in $CONTRACTS; do
        log "warm-up: $c at 8 jobs for ${WARMUP}s (discarded)"
        run_fio "$c" shared 8 "$RESULT_DIR/warmup_${c}.json"
    done
fi

# ---------------------------------------------------------------------------
# Build the point list, then run REPETITIONS AS THE OUTER LOOP with the order
# reshuffled each pass (M6). Point order must not correlate with the variable,
# or residual drift is indistinguishable from a scaling curve -- which is
# exactly how this project produced a fake one in August.
# ---------------------------------------------------------------------------
points=()
for c in $CONTRACTS; do
    for l in $LAYOUTS; do
        for nj in $NUMJOBS; do
            points+=("${c}:${l}:${nj}")
        done
    done
done

total_runs=$(( ${#points[@]} * REPS ))
log "${#points[@]} points x $REPS reps = $total_runs runs, "\
"~$(( total_runs * (RUNTIME + RAMP) / 60 )) min plus warm-up"

run_no=0
for rep in $(seq 1 "$REPS"); do
    log "--- repetition $rep/$REPS (order reshuffled) ---"
    while IFS= read -r point; do
        contract=${point%%:*}; rest=${point#*:}
        layout=${rest%%:*}
        nj=${rest#*:}
        tag="${contract}_${layout}_nj${nj}_rep${rep}"
        run_no=$(( run_no + 1 ))

        # Between points, always. Otherwise run N reads what run N-1 left in
        # the page cache -- which distorts the buffered arm most, and the
        # buffered arm is the one this whole script exists to produce.
        "$HERE/bench_hygiene.sh" drop >/dev/null 2>&1

        log "[$run_no/$total_runs] $tag"
        start_observers "$tag" "$(( RUNTIME + RAMP ))"
        run_fio "$contract" "$layout" "$nj" "$RESULT_DIR/${tag}.json"
        rc=$?
        stop_observers
        [ "$rc" -ne 0 ] && log "  fio exit $rc -- see $LOG"
        summarise "$tag" | tee -a "$LOG"

        # THE CONTROL. With O_DIRECT there is no cache between fio and the
        # device, so what the application submitted and what the device
        # received must agree. When they do not, the MEASUREMENT is wrong --
        # not the kernel -- and every other number in the run inherits that.
        # Checked automatically because the first smoke run failed it and it
        # took a human reading thirty lines to notice.
        if [ "$contract" = "direct" ]; then
            python3 - "$RESULT_DIR" "$tag" "$RAMP" <<'PY' | tee -a "$LOG"
import json, os, sys, statistics
d, tag, skip = sys.argv[1], sys.argv[2], int(sys.argv[3])
try:
    app = json.load(open(os.path.join(d, tag + ".json")))["jobs"][0]["write"]["bw_bytes"] / 1e6
    hdr, rows = None, []
    for line in open(os.path.join(d, tag + ".iostat")):
        f = line.split()
        if f and f[0] == "Device":
            hdr = {n: i for i, n in enumerate(f)}
        elif hdr and len(f) > 3:
            rows.append(f)
    rows = rows[skip:] or rows
    key = "wMB/s" if "wMB/s" in hdr else "wkB/s"
    dev = statistics.median(float(r[hdr[key]]) for r in rows) / (1.0 if key == "wMB/s" else 1024.0)
    r = app / dev if dev else 0.0
    if r and abs(r - 1.0) > 0.15:
        pct = (1 / r - 1) * 100 if r < 1 else (r - 1) * 100
        print("  !! CONTROL FAILED: direct app/dev = %.2f, expected 1.00 +/- 0.15." % r)
        print("  !! Device wrote %.0f%% %s than the application submitted."
              % (pct, "more" if r < 1 else "less"))
        print("  !! Measurement problem, not a kernel result. Do not quote this run.")
except Exception:
    pass
PY
        fi

        # ---------------------------------------------------------------
        # The profile that actually answers the i_rwsem question. Throughput
        # curves can only SHOW a plateau; they cannot say which lock caused
        # it, and attributing it without this is the error PERFORMANCE.md
        # section 5 warns about by name.
        #
        # Read the output looking for:
        #   rwsem_down_write_slowpath / down_write   -> i_rwsem, the inode lock
        #   xas_* / xa_* / __filemap_*               -> the page cache's XArray
        #   native_queued_spin_lock_slowpath         -> some other spinlock;
        #                                               find its caller before
        #                                               claiming anything
        # Captured once per contract+layout, at the highest thread count only,
        # because that is where contention is visible and 150 profiles is a
        # data-management problem rather than a measurement.
        # ---------------------------------------------------------------
        if [ "$PROFILE_JOBS" != "0" ] && [ "$nj" = "$PROFILE_JOBS" ] && \
           [ "$rep" = "1" ] && command -v perf >/dev/null; then
            prof="$RESULT_DIR/${contract}_${layout}_profile"
            log "  profiling ${contract}/${layout} at $nj jobs (20s)"
            start_observers "${tag}_prof" 30
            run_fio "$contract" "$layout" "$nj" "$RESULT_DIR/${tag}_prof.json" &
            fio_bg=$!
            sleep 8   # let it reach steady state before sampling
            perf record -F 99 -a -g -o "${prof}.data" -- sleep 20 >>"$LOG" 2>&1
            wait "$fio_bg" 2>/dev/null
            stop_observers
            # Reduce to text immediately and drop the binary: perf.data for 6
            # profiles is gigabytes, and it lives on the device under test.
            perf report -i "${prof}.data" --stdio --sort symbol 2>/dev/null \
                | head -60 > "${prof}.txt"
            rm -f "${prof}.data"
            log "  -> ${prof}.txt"
            grep -iE 'rwsem|down_write|xas_|__filemap|queued_spin' "${prof}.txt" \
                | head -8 | sed 's/^/    /' | tee -a "$LOG"
        fi
    done < <(printf '%s\n' "${points[@]}" | shuf)
done

"$HERE/bench_hygiene.sh" record > "$RESULT_DIR/provenance.md" 2>&1
log "provenance -> $RESULT_DIR/provenance.md"
log "NOTE: fill the noxdb commit field by hand -- the bench box is not a checkout."
log "baseline sweep DONE"
log ""
log "Next: ./tools/baseline_report.py $RESULT_DIR"
