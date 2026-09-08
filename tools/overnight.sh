#!/usr/bin/env bash
#
# overnight.sh - the unattended block: repro verdicts, then M2 + M5.
#
# ORDER IS LOAD-BEARING. The repro runs FIRST because preconditioning writes
# over the whole filesystem and would destroy its test file; and M5 runs LAST
# because a queue-depth sweep taken on a fresh drive measures the pSLC cache
# rather than the device (C10E-METHOD M2).
#
#   1. hygiene apply         swap off, governor, caches   (M3)
#   2. order_repro x2        the two ordering verdicts     (minutes)
#   3. precondition          fill + overwrite past the SLC knee (M2)
#   4. fio sweep             the SN530 ceiling             (M5)
#   5. record                provenance block              (for PERFORMANCE.md)
#
# Everything is logged with timestamps under RESULT_DIR. Nothing here is
# interactive; read the logs in the morning.
#
# DESTRUCTIVE. Step 3 writes a file sized to most of the free space on
# $NVME_MOUNT and overwrites it repeatedly. It does not delete your existing
# .dat files, but it WILL consume the free space around them for the duration
# and leaves a large PRECOND_FILE behind (removed at the end unless KEEP=1).
#
# BUILD FIRST, as your normal user -- this script does not compile anything, so
# that root never owns the object files:
#     make repro-order && make repro-order-multi
# Then:
#     sudo ./tools/overnight.sh
#
# Knobs: PASSES (precondition overwrites, default 2), RUNTIME (seconds per fio
# point, default 120), REPS (fio repetitions per point, default 5 -- M4 asks for
# a minimum of 5), FILL_PCT (percent of free space to precondition, default 80).

set -uo pipefail    # deliberately NOT -e: one failing fio point must not throw
                    # away the whole night's remaining work.

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$HERE")

NVME_MOUNT=${NVME_MOUNT:-/mnt/nvme}
RESULT_DIR=${RESULT_DIR:-$NVME_MOUNT/results/$(date +%Y%m%d-%H%M%S)}
PRECOND_FILE=${PRECOND_FILE:-$NVME_MOUNT/precond.bin}
FIO_FILE=${FIO_FILE:-$NVME_MOUNT/fio_test.bin}

PASSES=${PASSES:-2}
RUNTIME=${RUNTIME:-120}
REPS=${REPS:-5}
FILL_PCT=${FILL_PCT:-80}
KEEP=${KEEP:-0}
SWEEP_QDS=${SWEEP_QDS:-"1 2 4 8 16 32 64"}
# Idle interval between the last preconditioning write and the first measured
# one. 600s is not a guess dressed as a constant: the 2026-08-20 QD1 point took
# roughly four 120s repetitions (~8 min of load plus gaps) to stop climbing,
# so this is that observed recovery time with margin.
SETTLE=${SETTLE:-600}

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$RESULT_DIR/overnight.log"; }

[ "$(id -u)" -eq 0 ] || { echo "must run as root: sudo $0" >&2; exit 1; }

# --- fail loudly NOW, not six hours in ------------------------------------
command -v fio >/dev/null || { echo "fio not installed: apt install fio" >&2; exit 1; }
mountpoint -q "$NVME_MOUNT" || { echo "$NVME_MOUNT not mounted" >&2; exit 1; }
for b in order_repro order_repro_multi; do
    [ -x "$REPO/bench/$b" ] || {
        echo "missing $REPO/bench/$b -- run 'make repro-order && make repro-order-multi' first" >&2
        exit 1; }
done

mkdir -p "$RESULT_DIR"
log "results -> $RESULT_DIR"

# --- 1. hygiene ------------------------------------------------------------
log "=== M3 hygiene ==="
"$HERE/bench_hygiene.sh" apply 2>&1 | tee -a "$RESULT_DIR/overnight.log"
"$HERE/bench_hygiene.sh" check 2>&1 | tee -a "$RESULT_DIR/overnight.log"

# --- 2. ordering repro -----------------------------------------------------
# Runs before preconditioning: it needs its own small file, which step 3 would
# otherwise bury. Exit codes are recorded, not acted on -- a FAIL from the
# second build is the EXPECTED result and must not stop the night.
log "=== C4 ordering repro ==="
"$HERE/bench_hygiene.sh" drop >/dev/null
"$REPO/bench/order_repro" "$NVME_MOUNT/order.dat" > "$RESULT_DIR/repro_stage2_1.txt" 2>&1
log "repro (1 Stage-2 thread, predicted PASS): exit $?"

"$HERE/bench_hygiene.sh" drop >/dev/null
"$REPO/bench/order_repro_multi" "$NVME_MOUNT/order.dat" > "$RESULT_DIR/repro_stage2_4.txt" 2>&1
log "repro (4 Stage-2 threads, predicted FAIL): exit $?"
rm -f "$NVME_MOUNT/order.dat"

# --- 3. M2 preconditioning -------------------------------------------------
# A TLC drive with a dynamic pSLC cache serves the first tens of GB at burst
# speed. Every NoxDB number recorded so far carries a "this is SLC" caveat
# (C1-GATE, C3-GATE). Filling and then OVERWRITING the drive forces the
# controller past that cache so the sweep below sees sustained TLC.
avail_mb=$(df -BM --output=avail "$NVME_MOUNT" | tail -1 | tr -dc '0-9')
fill_mb=$(( avail_mb * FILL_PCT / 100 ))
log "=== M2 precondition: ${fill_mb} MiB x ${PASSES} passes (avail ${avail_mb} MiB) ==="

for pass in $(seq 1 "$PASSES"); do
    log "precondition pass $pass/$PASSES"
    fio --name=precond --filename="$PRECOND_FILE" --size="${fill_mb}M" \
        --rw=write --bs=1M --direct=1 --ioengine=libaio --iodepth=32 \
        --group_reporting --output-format=json \
        --output="$RESULT_DIR/precond_pass${pass}.json" \
        >> "$RESULT_DIR/overnight.log" 2>&1
    log "  pass $pass done (exit $?)"
done

# --- 4. M5 fio sweep -------------------------------------------------------
# The hardware ceiling. Without it there is no way to interpret a single NoxDB
# number: "1.8 GB/s" means nothing until you know what the device can do.
#
# Delegated to fio_sweep.sh, which shuffles the point order and settles first.
# The 2026-08-20 run had this sweep inline with nested loops and no settle, and
# it produced a recovery transient inside randwrite4k QD1 -- see the header of
# fio_sweep.sh for the numbers and the argument. Do not re-inline it.
log "=== M5 sweep (delegated to fio_sweep.sh, shuffled + settled) ==="
RESULT_DIR="$RESULT_DIR" REPS="$REPS" RUNTIME="$RUNTIME" SETTLE="$SETTLE" \
FIO_FILE="$FIO_FILE" \
    "$HERE/fio_sweep.sh" "$RESULT_DIR" "$SWEEP_QDS" 2>&1 | tee -a "$RESULT_DIR/overnight.log"

if [ "$KEEP" != "1" ]; then
    rm -f "$PRECOND_FILE" "$FIO_FILE"
    log "removed preconditioning and fio scratch files (KEEP=1 to keep them)"
fi

log "=== DONE ==="
echo
echo "Morning checklist:"
echo "  1. $RESULT_DIR/repro_stage2_1.txt   -> must say VERDICT: PASS"
echo "  2. $RESULT_DIR/repro_stage2_4.txt   -> must say VERDICT: FAIL, stale=N"
echo "  3. grep -h '\"bw_bytes\"' $RESULT_DIR/randwrite4k_*.json | head"
echo "  4. $RESULT_DIR/provenance.md        -> paste into PERFORMANCE.md"
