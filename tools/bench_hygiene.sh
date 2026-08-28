#!/usr/bin/env bash
#
# bench_hygiene.sh - C10E-METHOD M3 (run hygiene), plus the provenance block.
#
# Bench box only (bare-metal Ubuntu 24.04). Every item here has invalidated a
# published benchmark at some point, which is why M3 is listed as blocking:
#
#   swap off      swap lives on the USB OS disk on this box. A single page out
#                 during a run and the number is about the USB bus, not the NVMe.
#   governor      ondemand ramps the clock DURING the measurement, so the first
#                 seconds of every run are slower than the rest and p99 inherits
#                 a transition that has nothing to do with the engine.
#   drop_caches   without it, run N reads what run N-1 left in the page cache.
#                 The BUFFERED baseline is the one this distorts most, and it is
#                 the baseline the whole E2 comparison rests on.
#   fallocate     writing into a sparse file measures XFS extent allocation
#                 mixed into the write path. Preallocating moves that cost out.
#
# Pinning is the fifth M3 item and this script CANNOT do it: it has to happen
# inside the process. Use taskset on the binary (taskset -c 0-15 ./bench/...)
# or pthread_setaffinity_np in the driver -- do not assume it is handled here.
#
# USAGE
#   sudo ./tools/bench_hygiene.sh check            # report only, changes nothing
#   sudo ./tools/bench_hygiene.sh apply            # swap off + governor + caches
#   sudo ./tools/bench_hygiene.sh drop             # drop caches only (between runs)
#   sudo ./tools/bench_hygiene.sh prep FILE SIZE_MB  # fallocate a test file
#   sudo ./tools/bench_hygiene.sh record           # provenance block for PERFORMANCE.md
#   sudo ./tools/bench_hygiene.sh restore          # undo apply
#
# `apply` saves the previous swap and governor state to STATE_FILE so `restore`
# puts the box back. It does NOT touch the contents of /mnt/nvme: device
# preconditioning is C10E-METHOD M2, it is destructive, and it deliberately
# lives in a separate step so it can never run as a side effect of this one.

set -euo pipefail

STATE_FILE=${STATE_FILE:-/var/tmp/noxdb_bench_hygiene.state}
NVME_MOUNT=${NVME_MOUNT:-/mnt/nvme}

die()  { echo "bench_hygiene: $*" >&2; exit 1; }
note() { echo "  $*"; }

need_root() {
    [ "$(id -u)" -eq 0 ] || die "must run as root (sudo $0 $*)"
}

# Resolve the block device backing $NVME_MOUNT, stripped to the parent disk
# (nvme0n1p1 -> nvme0n1) so the sysfs queue knobs below resolve.
nvme_disk() {
    local src
    src=$(findmnt -no SOURCE "$NVME_MOUNT" 2>/dev/null) || return 1
    lsblk -no PKNAME "$src" 2>/dev/null | head -1
}

cmd_check() {
    echo "=== hygiene check ==="

    local swaptotal
    swaptotal=$(awk '/^SwapTotal:/ {print $2}' /proc/meminfo)
    if [ "$swaptotal" -eq 0 ]; then
        note "swap:      OFF (SwapTotal 0 kB) OK"
    else
        note "swap:      ON, ${swaptotal} kB  <-- FAIL, run 'apply'"
    fi

    local govs
    govs=$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | tr '\n' ' ' || true)
    if [ -z "$govs" ]; then
        note "governor:  no cpufreq sysfs (BIOS/driver may own P-states)"
    elif [ "$govs" = "performance " ]; then
        note "governor:  performance OK"
    else
        note "governor:  ${govs} <-- FAIL, run 'apply'"
    fi

    if mountpoint -q "$NVME_MOUNT"; then
        note "mount:     $NVME_MOUNT $(findmnt -no SOURCE,FSTYPE "$NVME_MOUNT")"
    else
        note "mount:     $NVME_MOUNT NOT MOUNTED <-- every target path is wrong"
    fi

    # The OS disk is USB on this box. Measuring it instead of the NVMe is the
    # single easiest way to produce a beautiful, meaningless graph.
    local disk
    if disk=$(nvme_disk) && [ -n "$disk" ]; then
        note "device:    /dev/$disk  model=$(cat "/sys/block/$disk/device/model" 2>/dev/null | xargs || echo '?')"
        note "scheduler: $(cat "/sys/block/$disk/queue/scheduler" 2>/dev/null || echo '?')"
    else
        note "device:    could not resolve backing device for $NVME_MOUNT"
    fi

    note "memory:    $(awk '/^MemTotal:/ {printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
}

cmd_apply() {
    need_root apply
    echo "=== applying hygiene ==="

    : > "$STATE_FILE"

    # --- swap ---
    if [ "$(awk '/^SwapTotal:/ {print $2}' /proc/meminfo)" -ne 0 ]; then
        echo "swap_was_on=1" >> "$STATE_FILE"
        swapoff -a
        note "swap: off"
    else
        echo "swap_was_on=0" >> "$STATE_FILE"
        note "swap: already off"
    fi
    [ "$(awk '/^SwapTotal:/ {print $2}' /proc/meminfo)" -eq 0 ] \
        || die "swapoff ran but SwapTotal is still non-zero -- do not measure"

    # --- governor ---
    local any=0 g
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ -e "$g" ] || continue
        [ "$any" -eq 0 ] && { echo "governor_was=$(cat "$g")" >> "$STATE_FILE"; any=1; }
        echo performance > "$g"
    done
    if [ "$any" -eq 1 ]; then
        note "governor: performance"
    else
        note "governor: no cpufreq sysfs; nothing to set (check BIOS P-states)"
    fi

    cmd_drop
    echo
    note "NOTE: pinning is NOT handled here -- use taskset on the binary."
}

cmd_drop() {
    need_root drop
    sync
    echo 3 > /proc/sys/vm/drop_caches
    note "page cache, dentries and inodes dropped"
}

cmd_prep() {
    need_root prep
    local file=${1:?usage: prep FILE SIZE_MB} mb=${2:?usage: prep FILE SIZE_MB}

    # Refuse to preallocate outside the NVMe mount. The OS disk is a USB-attached
    # Kingston NV2 on this box; a mistyped path there does not fail loudly, it
    # just silently produces numbers for the wrong device.
    case "$file" in
        "$NVME_MOUNT"/*) ;;
        *) die "refusing to prep '$file': not under $NVME_MOUNT" ;;
    esac

    # Destructive, and quietly so if the file is an earlier run's data. Say what
    # is being replaced rather than discovering it later in a confusing result.
    if [ -e "$file" ]; then
        note "replacing existing $file ($(du -h "$file" | cut -f1))"
        rm -f "$file"
    fi
    fallocate -l "${mb}M" "$file"
    note "fallocated $file = ${mb} MiB (extents allocated up front, not during the run)"
}

cmd_record() {
    echo "### Provenance"
    echo
    echo "- date: $(date -Is)"
    echo "- host: $(hostname)"
    echo "- kernel: $(uname -sr)"
    echo "- cpu: $(awk -F: '/model name/ {print $2; exit}' /proc/cpuinfo | xargs)"
    echo "- cores: $(nproc)"
    echo "- memory: $(awk '/^MemTotal:/ {printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
    echo "- swap: $(awk '/^SwapTotal:/ {print $2}' /proc/meminfo) kB (must be 0)"
    echo "- governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'n/a')"
    local disk
    if disk=$(nvme_disk) && [ -n "$disk" ]; then
        echo "- device: /dev/$disk ($(cat "/sys/block/$disk/device/model" 2>/dev/null | xargs || echo '?'))"
        echo "- scheduler: $(cat "/sys/block/$disk/queue/scheduler" 2>/dev/null || echo '?')"
        echo "- nr_requests: $(cat "/sys/block/$disk/queue/nr_requests" 2>/dev/null || echo '?')"
        echo "- write_cache: $(cat "/sys/block/$disk/queue/write_cache" 2>/dev/null || echo '?')"
    fi
    echo "- mount: $(findmnt -no SOURCE,FSTYPE,OPTIONS "$NVME_MOUNT" 2>/dev/null || echo 'NOT MOUNTED')"
    echo "- noxdb commit: $(git -C "$(dirname "$0")/.." rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo
    echo "Memory note: this box went to 1x16 GB on 2026-08-17 (single channel)."
    echo "Any comparison against a number recorded before that date is invalid."
}

cmd_restore() {
    need_root restore
    [ -f "$STATE_FILE" ] || die "no state file at $STATE_FILE; nothing to restore"
    # shellcheck disable=SC1090
    . "$STATE_FILE"

    if [ "${swap_was_on:-0}" = "1" ]; then
        swapon -a && note "swap: back on"
    else
        note "swap: was already off before apply; left off"
    fi

    if [ -n "${governor_was:-}" ]; then
        local g
        for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            [ -e "$g" ] && echo "$governor_was" > "$g"
        done
        note "governor: restored to $governor_was"
    fi
    rm -f "$STATE_FILE"
}

case "${1:-}" in
    check)   cmd_check ;;
    apply)   cmd_apply ;;
    drop)    cmd_drop ;;
    prep)    shift; cmd_prep "$@" ;;
    record)  cmd_record ;;
    restore) cmd_restore ;;
    *) sed -n '4,40p' "$0"; exit 2 ;;
esac
