#!/usr/bin/env bash
#
# fio_sweep.sh - C10E-METHOD M5 (hardware ceiling) + M4 (repetition).
#
# WHY THIS IS A SEPARATE SCRIPT, AND WHY IT SHUFFLES
# -------------------------------------------------
# The 2026-08-20 sweep ran its points in nested loops -- shape, then queue
# depth, then repetition -- so the FIRST point measured was randwrite4k QD1,
# taken minutes after 748 GiB of preconditioning while the controller was still
# doing garbage collection. Its five repetitions came out monotonically
# increasing:
#
#     58.2 -> 87.7 -> 129.3 -> 252.2 -> 253.0 MB/s
#
# That is not variance, it is a recovery transient, and a nested loop puts the
# whole transient inside a single QD. Execution order was confounded with the
# independent variable: the lowest queue depth was systematically measured on
# the least recovered drive, which is exactly the shape of a fake "scaling"
# curve. The QD2..QD64 points, spread over the following two hours, agreed to
# better than 1% -- so the plateau survived, but QD1 did not.
#
# Two fixes, both here:
#   SETTLE  idle time after preconditioning, so GC finishes before measuring.
#   SHUFFLE repetitions are the OUTER loop and the point order is reshuffled
#           each pass, so every queue depth is sampled early, late and in the
#           middle. Residual drift then shows up as SPREAD within a point
#           instead of as slope across the curve -- visible rather than
#           disguised as a result.
#
# ALWAYS INCLUDE AN ANCHOR. When re-measuring a subset (say QD1 alone), pass a
# queue depth that a previous run already measured cleanly. If the anchor
# reproduces, the new points are comparable to the old ones; if it does not,
# the drive changed and nothing may be compared across runs.
#
# USAGE
#   sudo ./tools/fio_sweep.sh RESULT_DIR "QD LIST" [SHAPES]
#
#   sudo ./tools/fio_sweep.sh /mnt/nvme/results/rerun "1 2"
#   sudo ./tools/fio_sweep.sh /mnt/nvme/results/full "1 2 4 8 16 32 64"
#
# Env: REPS (5), RUNTIME (120), RAMP (10), SETTLE (0), FIO_FILE, FIO_SIZE (16G)
# Shapes are "name:rw:bs" triples; default is the two the engine actually has a
# path for -- 4K random (scrap path) and 1M sequential (fast path).

set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)

RESULT_DIR=${1:?usage: fio_sweep.sh RESULT_DIR "QD LIST" [SHAPES]}
QDS=${2:?usage: fio_sweep.sh RESULT_DIR "QD LIST" [SHAPES]}
SHAPES=${3:-"randwrite4k:randwrite:4k seqwrite1m:write:1M"}

REPS=${REPS:-5}
RUNTIME=${RUNTIME:-120}
RAMP=${RAMP:-10}
SETTLE=${SETTLE:-0}
# Per-shape conditioning before the first measured point. 300s is not a round
# number picked for comfort: 1M sequential was still climbing after five 120s
# repetitions, so anything shorter demonstrably fails to reach steady state on
# that shape. See the WARM-UP block below.
WARMUP=${WARMUP:-300}
FIO_FILE=${FIO_FILE:-/mnt/nvme/fio_test.bin}
FIO_SIZE=${FIO_SIZE:-16G}
ACTIVE=${ACTIVE:-1}          # 1 = observe the device while the load runs
NVME_MOUNT=${NVME_MOUNT:-/mnt/nvme}

[ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 1; }
command -v fio >/dev/null || { echo "fio not installed" >&2; exit 1; }

# Parent disk backing the mount (nvme0n1p1 -> nvme0n1), for the iostat capture.
DEV=""
if [ "$ACTIVE" = "1" ]; then
    if command -v iostat >/dev/null; then
        DEV=$(lsblk -no PKNAME "$(findmnt -no SOURCE "$NVME_MOUNT")" 2>/dev/null | head -1)
        [ -n "$DEV" ] || echo "warn: cannot resolve device for $NVME_MOUNT; active observation off" >&2
    else
        echo "warn: iostat missing (apt install sysstat); active observation off" >&2
    fi
fi

mkdir -p "$RESULT_DIR"
LOG="$RESULT_DIR/sweep.log"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

log "sweep: qds=[$QDS] reps=$REPS runtime=${RUNTIME}s ramp=${RAMP}s settle=${SETTLE}s"
log "shapes: $SHAPES"

if [ "$SETTLE" -gt 0 ]; then
    # Idle, not busy-wait: the point is to give the controller an interval with
    # no host I/O so background GC from the preconditioning writes can retire.
    log "settling ${SETTLE}s before first measurement (GC drain)"
    sleep "$SETTLE"
fi

# --- WARM-UP -------------------------------------------------------------
# Discovered 2026-08-20 by the shuffle this script added. With point order
# randomised, the depressed samples stopped clustering inside one queue depth
# and revealed themselves for what they are: the first repetition of the first
# few points is slow REGARDLESS of which point that happens to be.
#
#     4K random  QD1:  53.2 -> 250.3 251.9 253.4 253.2   (rep1 is 4.7x low)
#     4K random  QD2:  79.5 -> 406.7 407.9 406.4 409.1   (rep1 is 5.1x low)
#     1M seq     QD1: 438.8 -> 479.9 493.6 516.8 511.7   (still climbing at rep5)
#
# The drive had been idle for six hours, so this is not garbage collection from
# preconditioning -- it is a start-of-load transient, and fio's --ramp_time=10
# is roughly two orders of magnitude short of covering it.
#
# Note the two shapes have DIFFERENT time constants: 4K random converges within
# one repetition, 1M sequential had not converged after five (an anchor point
# re-measured here came out 22% below the same point measured at the END of a
# two-hour sweep). One ramp value cannot serve both, so each shape gets its own
# warm-up job, run at the deepest queue in the sweep to reach steady state
# fastest. The output is discarded on purpose: this is conditioning, not data.
if [ "$WARMUP" -gt 0 ]; then
    warm_qd=$(printf '%s\n' $QDS | sort -n | tail -1)
    for shape in $SHAPES; do
        wname=${shape%%:*}; wrest=${shape#*:}
        wrw=${wrest%%:*};   wbs=${wrest#*:}
        log "warm-up: $wname at QD$warm_qd for ${WARMUP}s (discarded)"
        fio --name="warmup_$wname" --filename="$FIO_FILE" --size="$FIO_SIZE" \
            --rw="$wrw" --bs="$wbs" --direct=1 --ioengine=libaio \
            --iodepth="$warm_qd" --runtime="$WARMUP" --time_based \
            --output=/dev/null >> "$LOG" 2>&1
        log "  warm-up $wname done"
    done
fi

# Build the full point list once, then reshuffle it per repetition.
points=()
for shape in $SHAPES; do
    for qd in $QDS; do
        points+=("$shape:$qd")
    done
done
log "${#points[@]} points x $REPS reps = $(( ${#points[@]} * REPS )) runs, "\
"~$(( ${#points[@]} * REPS * (RUNTIME + RAMP) / 60 )) min"

for rep in $(seq 1 "$REPS"); do
    log "--- repetition $rep/$REPS (order reshuffled) ---"
    while IFS= read -r point; do
        name=${point%%:*}; rest=${point#*:}
        rw=${rest%%:*};    rest=${rest#*:}
        bs=${rest%%:*}
        qd=${rest#*:}

        tag="${name}_qd${qd}_rep${rep}"
        "$HERE/bench_hygiene.sh" drop >/dev/null 2>&1

        # ACTIVE BENCHMARKING (Gregg, Systems Performance ch. 12). Reading only
        # fio's own output is PASSIVE: it tells you what fio believes happened
        # and nothing about whether the system agrees. Observing the device from
        # a second angle WHILE the load runs is what turns a number into
        # evidence, and it answers two questions the JSON cannot:
        #
        #   %util   is the device actually saturated, or is the bottleneck
        #           somewhere between fio and the NAND?
        #   aqu-sz  does the queue depth the DEVICE sees match the --iodepth we
        #           asked for? If it does not, something is merging, splitting
        #           or throttling the requests and the x-axis is a fiction.
        #
        # This is also the measurement the engine's own runs will need later:
        # NoxDB's effective queue depth is exactly this number under its load.
        if [ "$ACTIVE" = "1" ] && [ -n "$DEV" ]; then
            iostat -x -y 1 "$RUNTIME" "/dev/$DEV" \
                > "$RESULT_DIR/${tag}.iostat" 2>/dev/null &
            iostat_pid=$!
        else
            iostat_pid=""
        fi

        fio --name="$tag" --filename="$FIO_FILE" --size="$FIO_SIZE" \
            --rw="$rw" --bs="$bs" --direct=1 --ioengine=libaio \
            --iodepth="$qd" --runtime="$RUNTIME" --time_based \
            --ramp_time="$RAMP" --group_reporting --output-format=json \
            --output="$RESULT_DIR/${tag}.json" >> "$LOG" 2>&1
        rc=$?

        obs=""
        if [ -n "$iostat_pid" ]; then
            wait "$iostat_pid" 2>/dev/null
            # Median %util and aqu-sz across the sampled seconds. Median, not
            # mean: the ramp and the tail-off would drag a mean without saying
            # so. Column positions are read from the header rather than
            # hardcoded -- iostat's layout differs across sysstat versions, and
            # a fixed $12 silently reports the wrong metric on the wrong box.
            obs=$(awk -v d="$DEV" '
                /^Device/ { for (i=1;i<=NF;i++) { if ($i=="aqu-sz") a=i; if ($i=="%util") u=i } next }
                $1==d && a && u { qs[++n]=$a; us[n]=$u }
                END {
                    if (!n) { print "(no samples)"; exit }
                    asort(qs); asort(us); m=int((n+1)/2)
                    printf "aqu-sz=%.2f %%util=%.1f", qs[m], us[m]
                }' "$RESULT_DIR/${tag}.iostat" 2>/dev/null)
            [ -n "$obs" ] && obs="  [$obs]"
        fi

        # Record the wall clock of every point. Without it a drift artifact like
        # the one this script exists to prevent cannot be detected after the
        # fact -- you would have the numbers but not their order.
        log "  $tag exit $rc at $(date +%H:%M:%S)$obs"
    done < <(printf '%s\n' "${points[@]}" | shuf)
done

"$HERE/bench_hygiene.sh" record > "$RESULT_DIR/provenance.md" 2>&1
log "provenance -> $RESULT_DIR/provenance.md"
log "sweep DONE"
