#!/usr/bin/env bash
#
# today.sh - one unattended launch, analysed later from anywhere.
#
# WHY THIS EXISTS. fstrim.timer fired at 2026-08-25 19:39 UTC and discarded
# 460.6 GiB on /mnt/nvme, so the drive is no longer in the sustained-TLC state
# that every §3 number in PERFORMANCE.md was measured in. Anything measured
# before re-preconditioning would be reading the pSLC cache and would not be
# comparable to §3. Hence step 2 below, and hence step 1: the NEXT scheduled
# TRIM is 2026-08-31, which would silently do this again mid-campaign with
# nobody physically at the box.
#
# ORDER IS LOAD-BEARING:
#   1. mask fstrim      stop the 08-31 timer from repeating this
#   2. hygiene apply    swap off (it lives on the USB OS disk), governor
#   3. precondition     2 passes past the pSLC knee            (~30 min)
#   4. 4K sweep         §3.1 with device-side observation      (~1h26)
#   5. soak + observe   engine aqu-sz + perf stat              (~6 min)
#
# Step 4 before step 5 on purpose. The soak is the step that can OOM, and an
# OOM kills the whole systemd unit -- on 2026-08-25 it took down a sweep that
# had not started yet. The long, irreplaceable job runs first.
#
#     sudo ./tools/today.sh          (or: nohup sudo ./tools/today.sh &)
#
# Everything lands under $RESULT_ROOT with timestamps. Read the logs later.

set -uo pipefail    # NOT -e: a failed point must not discard the rest.

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$HERE")

NVME_MOUNT=${NVME_MOUNT:-/mnt/nvme}
RESULT_ROOT=${RESULT_ROOT:-$NVME_MOUNT/results/$(date +%Y%m%d-%H%M%S)-today}
PRECOND_FILE=${PRECOND_FILE:-$NVME_MOUNT/precond.bin}
FIO_FILE=${FIO_FILE:-$NVME_MOUNT/fio_test.bin}

PASSES=${PASSES:-2}
FILL_PCT=${FILL_PCT:-80}
SKIP_PRECOND=${SKIP_PRECOND:-0}
SOAK_SECONDS=${SOAK_SECONDS:-300}
SOAK_THREADS=${SOAK_THREADS:-16}
SWEEP_QDS=${SWEEP_QDS:-"1 2 4 8 16 32 64"}
KEEP=${KEEP:-0}

[ "$(id -u)" -eq 0 ] || { echo "must run as root: sudo $0" >&2; exit 1; }
command -v fio >/dev/null || { echo "fio not installed" >&2; exit 1; }
mountpoint -q "$NVME_MOUNT" || { echo "$NVME_MOUNT not mounted" >&2; exit 1; }
# A soak binary is NOT self-describing. bench/otflush_soak built with
# NOX_WATERMARK=1000000000 -- thread 06's deliberately-disarmed "before" build --
# is a perfectly valid binary that OOM-kills this box in 7 seconds. That is
# exactly what happened at 20:11 on 2026-08-25 and it cost the whole run: the
# unit died in the OOM and the fio sweep after it never started. Checking that
# the file EXISTS is not checking that it is the build you meant. So build it
# here, with the knobs this measurement needs, and verify the stamp afterwards.
BUILD_USER=${SUDO_USER:-root}

mkdir -p "$RESULT_ROOT"
log() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$RESULT_ROOT/today.log"; }
log "results -> $RESULT_ROOT"

# --- 1. stop the next TRIM -------------------------------------------------
log "=== masking fstrim.timer (next fire would be 2026-08-31) ==="
systemctl mask --now fstrim.timer 2>&1 | tee -a "$RESULT_ROOT/today.log"
log "fstrim.timer is-enabled: $(systemctl is-enabled fstrim.timer 2>&1)"

# --- 2. hygiene (M3) -------------------------------------------------------
log "=== M3 hygiene ==="
"$HERE/bench_hygiene.sh" apply 2>&1 | tee -a "$RESULT_ROOT/today.log"
"$HERE/bench_hygiene.sh" check 2>&1 | tee -a "$RESULT_ROOT/today.log"

# --- 3. re-precondition (M2) ----------------------------------------------
# The 460.6 GiB TRIM returned the NAND toward clean. Fill + overwrite forces
# the controller past the dynamic pSLC cache back into sustained TLC. The
# evidence that it worked is the two passes agreeing: they came out 461.8 and
# 462.7 MB/s (0.2% apart) the last time this ran. Check that in the JSON.
avail_mb=$(df -BM --output=avail "$NVME_MOUNT" | tail -1 | tr -dc '0-9')
fill_mb=$(( avail_mb * FILL_PCT / 100 ))
log "=== M2 re-precondition: ${fill_mb} MiB x ${PASSES} passes (avail ${avail_mb} MiB) ==="
if [ "$SKIP_PRECOND" = "1" ]; then
    log "  SKIP_PRECOND=1 -- drive already conditioned this session; not redoing it"
fi
for pass in $(seq 1 "$PASSES"); do
    [ "$SKIP_PRECOND" = "1" ] && break
    log "precondition pass $pass/$PASSES"
    fio --name=precond --filename="$PRECOND_FILE" --size="${fill_mb}M" \
        --rw=write --bs=1M --direct=1 --ioengine=libaio --iodepth=32 \
        --group_reporting --output-format=json \
        --output="$RESULT_ROOT/precond_pass${pass}.json" \
        >> "$RESULT_ROOT/today.log" 2>&1
    log "  pass $pass done (exit $?)"
done
for pass in $(seq 1 "$PASSES"); do
    [ "$SKIP_PRECOND" = "1" ] && break
    bw=$(python3 -c "import json;print('%.1f'%(json.load(open('$RESULT_ROOT/precond_pass${pass}.json'))['jobs'][0]['write']['bw_bytes']/1e6))" 2>/dev/null)
    log "  pass $pass sustained: ${bw:-?} MB/s"
done

# ORDER NOTE (2026-08-25): the sweep now runs BEFORE the soak. The soak is
# the step that can OOM, and an OOM kills the entire systemd unit -- which is
# how the first attempt lost a 90-minute sweep that had not started yet. The
# long, irreplaceable job goes first.
# --- 4. 4K sweep with device-side observation (M5) -------------------------
# PERFORMANCE.md §5: "§3.1 and §3.3 have no device-side confirmation" -- the
# earlier sweeps predate sysstat being installed. 4 KiB is below
# max_sectors_kb so no request splitting is expected here, but "expected" is
# the word this project has been wrong about twice, which is the reason to
# look rather than to assume.
log "=== M5 4K sweep, active (qds=[$SWEEP_QDS]) ==="
REPS=${REPS:-5} RUNTIME=${RUNTIME:-120} WARMUP=${WARMUP:-300} SETTLE=${SETTLE:-300} \
FIO_FILE="$FIO_FILE" \
    "$HERE/fio_sweep.sh" "$RESULT_ROOT/rw4k-active" "$SWEEP_QDS" \
    "randwrite4k:randwrite:4k" 2>&1 | tee -a "$RESULT_ROOT/today.log"

# --- 5. engine effective queue depth + CPU efficiency ----------------------
# Settles the prediction recorded in PERFORMANCE.md §5 BEFORE this measurement:
#   ">= 4, and 8-16 whenever the pwritev batch fires."
# Stage-2 writes 256 KiB pages and batches up to 8 into one 2 MiB pwritev; at
# max_sectors_kb=128 the block layer splits those into 2 and 16 device
# requests. If aqu-sz confirms, a single Stage-2 thread already sits past the
# QD2-4 knee and raising NOX_STAGE2_THREADS buys nothing.
#
# perf stat rides along because it is free here: same load, second angle.
log "=== building soak binary, watermark ARMED (NOX_WATERMARK unset -> 4096) ==="
sudo -u "$BUILD_USER" make -C "$REPO" bench/otflush_soak >> "$RESULT_ROOT/today.log" 2>&1
# Field 5 of the Makefile's .entries-stamp is NOX_WATERMARK. Empty means the
# 4096-page default gate is compiled in; anything large means it is disarmed.
wm=$(cut -d"|" -f5 "$REPO/.entries-stamp" 2>/dev/null)
if [ -n "$wm" ] && [ "$wm" -gt 65536 ] 2>/dev/null; then
    log "REFUSING to soak: watermark is $wm pages, i.e. disarmed. This build OOMs the box."
    log "Rebuild with: make bench/otflush_soak NOX_WATERMARK="
    exit 1
fi
log "  watermark stamp: [${wm:-default 4096}] -- armed"

log "=== engine soak: aqu-sz + perf stat (${SOAK_SECONDS}s, ${SOAK_THREADS} threads) ==="
DEV=$(lsblk -no PKNAME "$(findmnt -no SOURCE "$NVME_MOUNT")" 2>/dev/null | head -1)
log "observing /dev/${DEV:-?}"
"$HERE/bench_hygiene.sh" drop >/dev/null 2>&1
if [ -n "$DEV" ] && command -v iostat >/dev/null; then
    iostat -x -y 1 "$((SOAK_SECONDS + 20))" "/dev/$DEV" \
        > "$RESULT_ROOT/soak.iostat" 2>/dev/null &
    iostat_pid=$!
else
    iostat_pid=""
    log "warn: no iostat/device -- soak runs WITHOUT the measurement it exists for"
fi

perf stat -e cycles,instructions,context-switches,cpu-migrations,page-faults \
    -o "$RESULT_ROOT/soak.perf" \
    "$REPO/bench/otflush_soak" "$NVME_MOUNT/c4soak.dat" \
    "$SOAK_SECONDS" "$SOAK_THREADS" \
    > "$RESULT_ROOT/soak.txt" 2>&1
log "  soak exit $?"
[ -n "$iostat_pid" ] && wait "$iostat_pid" 2>/dev/null

if [ -s "$RESULT_ROOT/soak.iostat" ]; then
    # Median, not mean: start-up and tail-off would drag a mean silently.
    # Header-driven column lookup -- iostat's layout moves across sysstat
    # versions and a hardcoded $12 reports the wrong metric without saying so.
    obs=$(gawk -v d="$DEV" '
        /^Device/ { for (i=1;i<=NF;i++) { if ($i=="aqu-sz") a=i; if ($i=="%util") u=i } next }
        $1==d && a && u { qs[++n]=$a; us[n]=$u }
        END { if (!n) { print "(no samples)"; exit }
              asort(qs); asort(us); m=int((n+1)/2)
              printf "aqu-sz=%.2f %%util=%.1f (n=%d)", qs[m], us[m], n }' \
        "$RESULT_ROOT/soak.iostat" 2>&1)
    log "  ENGINE QUEUE DEPTH: $obs"
    log "  prediction was >=4 (8-16 when pwritev batches) -- compare and SAY WHICH"
fi

[ "$KEEP" = "1" ] || rm -f "$PRECOND_FILE" "$FIO_FILE"

"$HERE/bench_hygiene.sh" record > "$RESULT_ROOT/provenance.md" 2>&1
log "=== DONE ==="
echo
echo "Read later, from anywhere:"
echo "  grep 'ENGINE QUEUE DEPTH' $RESULT_ROOT/today.log"
echo "  cat  $RESULT_ROOT/soak.perf"
echo "  ./tools/fio_report.py $RESULT_ROOT/rw4k-active --anchor=randwrite4k:2:410.0"
echo
echo "The anchor is the check that matters: if randwrite4k QD2 does not come"
echo "back near 410.0 MB/s, re-preconditioning did NOT restore the drive to"
echo "the state section 3 was measured in, and nothing may be compared to it."
