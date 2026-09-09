#!/usr/bin/env python3
"""
baseline_report.py - summarise a baseline_sweep run without letting the summary
become a lie.

    ./tools/baseline_report.py /mnt/nvme/results/baseline

WHAT THIS PRINTS THAT fio_report.py DOES NOT
--------------------------------------------
Two columns instead of one. For the buffered contracts, fio's bandwidth is the
rate the application filled the PAGE CACHE -- a memcpy rate -- while iostat's
is what the device actually received. Printing only the first would report a
storage number that never touched storage. So every point carries app, dev and
their ratio, and a ratio far from 1 is annotated rather than left for the
reader to notice.

THE SCALING VERDICT
-------------------
The question this sweep exists for is whether a buffered plateau across threads
survives when each thread writes its OWN file. i_rwsem is per-inode: under
`perjob` it cannot be the cause. So for each contract the report compares the
shared and perjob curves and says which of the three shapes it sees. It states
the shape; it does not name the lock. Naming the lock requires the perf
profiles, and the report points at them instead of guessing.

DRIFT is inherited from fio_report.py for the same reason it exists there: a
monotone series in execution order is a transient being averaged into a result.
"""
import glob
import json
import os
import re
import statistics
import sys


def med(v):
    return statistics.median(v) if v else 0.0


def read_meta(result_dir):
    """Sweep parameters, written by baseline_sweep.sh. The one that matters
    here is `ramp`: the observers ran alongside fio's discarded ramp, so those
    seconds must be dropped from the device-side median too, or the two halves
    of every row describe different intervals."""
    meta = {"ramp": 10}
    try:
        for line in open(os.path.join(result_dir, "sweep.meta")):
            if "=" in line:
                k, v = line.strip().split("=", 1)
                meta[k] = int(v) if v.isdigit() else v
    except Exception:
        pass
    return meta


def read_iostat(path, skip=0):
    """Device-side truth. Column positions come from the header, never fixed
    indices -- iostat's layout differs across sysstat versions and a hardcoded
    field silently reports the wrong metric."""
    out = {"dev_mbps": 0.0, "aqu": 0.0, "util": 0.0, "wareq": 0.0}
    try:
        hdr, rows = None, []
        for line in open(path):
            f = line.split()
            if not f:
                continue
            if f[0] == "Device":
                hdr = {n: i for i, n in enumerate(f)}
            elif hdr and len(f) > 3:
                rows.append(f)
        rows = rows[skip:] or rows
        if not (hdr and rows):
            return out

        def col(name):
            if name not in hdr:
                return []
            i = hdr[name]
            return [float(r[i]) for r in rows if len(r) > i]

        if "wMB/s" in hdr:
            out["dev_mbps"] = med(col("wMB/s"))
        elif "wkB/s" in hdr:
            out["dev_mbps"] = med(col("wkB/s")) / 1024.0
        out["aqu"] = med(col("aqu-sz"))
        out["util"] = med(col("%util"))
        out["wareq"] = med(col("wareq-sz"))
    except Exception:
        pass
    return out


def read_dirty(path):
    """Peak Dirty in MB. For the buffered arm this is the cache filling; if it
    flattens well below RAM, writeback throttling engaged and the curve bent
    for that reason rather than because of a lock."""
    try:
        vals = [int(l.split()[1]) for l in open(path) if len(l.split()) >= 3]
        return max(vals) // 1024 if vals else 0
    except Exception:
        return 0


def load(result_dir, skip=0):
    pts = {}
    for f in glob.glob(os.path.join(result_dir, "*_nj*_rep*.json")):
        base = os.path.basename(f)
        if "_prof" in base:
            continue
        m = re.match(r"(\w+)_(\w+)_nj(\d+)_rep(\d+)\.json$", base)
        if not m:
            continue
        contract, layout, nj, rep = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        try:
            w = json.load(open(f))["jobs"][0]["write"]
        except Exception as e:
            print("  ! unreadable: %s (%s)" % (base, e))
            continue
        tag = base[:-5]
        io = read_iostat(os.path.join(result_dir, tag + ".iostat"), skip)
        pts.setdefault((contract, layout, nj), []).append({
            "rep": rep,
            "app": w["bw_bytes"] / 1e6,
            "p99": w["clat_ns"]["percentile"].get("99.000000", 0) / 1000.0,
            "dirty": read_dirty(os.path.join(result_dir, tag + ".dirty")),
            **io,
        })
    for v in pts.values():
        v.sort(key=lambda r: r["rep"])
    return pts


CONTRACT_NOTE = {
    "buffered": "write() returns at the PAGE CACHE  (not a device measurement)",
    "bufsync":  "write() + periodic fsync -- returns at the DEVICE, on a cadence",
    "direct":   "write() returns at the DEVICE",
}


def drift(vals):
    """Same test fio_report.py uses: first sample also the extreme one, and the
    series monotone. That is a warm-up transient, not variance."""
    if len(vals) < 4:
        return None
    inc = all(b >= a for a, b in zip(vals, vals[1:]))
    dec = all(b <= a for a, b in zip(vals, vals[1:]))
    if inc and vals[0] == min(vals):
        return "MONOTONIC INCREASING -- warm-up transient, not variance"
    if dec and vals[0] == max(vals):
        return "MONOTONIC DECREASING -- device degrading through the point"
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    d = sys.argv[1]
    meta = read_meta(d)
    pts = load(d, skip=int(meta.get("ramp", 10)))
    if not pts:
        print("no points found in %s" % d)
        sys.exit(1)

    contracts = sorted({k[0] for k in pts})
    layouts = sorted({k[1] for k in pts})
    njs = sorted({k[2] for k in pts})

    print("\n=== baseline sweep: %s ===" % d)
    print("Contracts are reported SEPARATELY and are never averaged (M1).")
    print("runtime=%ss ramp=%ss (dropped from the device median) reps=%s "
          "working set=%s bs=%s"
          % (meta.get("runtime", "?"), meta.get("ramp", "?"),
             meta.get("reps", "?"), meta.get("total_size", "?"),
             meta.get("bs", "?")))
    print("app  = what fio measured   dev = what the device received (iostat)\n")

    curves = {}
    for c in contracts:
        print("-" * 78)
        print("CONTRACT: %s" % c)
        print("  %s" % CONTRACT_NOTE.get(c, ""))
        print("-" * 78)
        for l in layouts:
            print("\n  layout=%s" % l)
            absorbing = []
            print("    %5s %10s %10s %8s %11s %8s %7s %9s"
                  % ("jobs", "app MB/s", "dev MB/s", "app/dev", "p99 us",
                     "aqu-sz", "%util", "dirty MB"))
            for nj in njs:
                rows = pts.get((c, l, nj))
                if not rows:
                    continue
                app = med([r["app"] for r in rows])
                dev = med([r["dev_mbps"] for r in rows])
                ratio = app / dev if dev > 0 else 0.0
                print("    %5d %10.1f %10.1f %8.2f %11.1f %8.2f %7.1f %9d"
                      % (nj, app, dev, ratio,
                         med([r["p99"] for r in rows]),
                         med([r["aqu"] for r in rows]),
                         med([r["util"] for r in rows]),
                         max(r["dirty"] for r in rows)))
                curves[(c, l)] = curves.get((c, l), []) + [(nj, dev, app)]

                spread = 0.0
                vals = [r["app"] for r in rows]
                if len(vals) > 1 and med(vals) > 0:
                    spread = (max(vals) - min(vals)) / med(vals) * 100
                warn = drift(vals)
                if spread > 10 or warn:  # noqa: E501 - kept adjacent to the row it annotates
                    print("          reps in execution order: %s"
                          % ", ".join("%.1f" % v for v in vals))
                    if spread > 10:
                        print("          SPREAD %.1f%% -- the median is doing a lot of work here"
                              % spread)
                    if warn:
                        print("          DRIFT: %s" % warn)

                if ratio > 1.5:
                    absorbing.append((nj, ratio))

            # Said once per block, not once per row. On the buffered arm every
            # row trips this, and a warning repeated five times reads as
            # decoration rather than as the finding it is.
            if absorbing:
                print("      app/dev is %.2f-%.2fx across %d of these points: fio is"
                      % (min(r for _, r in absorbing), max(r for _, r in absorbing),
                         len(absorbing)))
                print("      measuring the page cache absorbing the load, NOT storage")
                print("      throughput. Only the dev column is a storage number here.")

    # ---------------------------------------------------------------
    # THE CONTROL, checked before any verdict is offered.
    #
    # O_DIRECT puts no cache between the application and the device, so app
    # and dev must agree. If they do not, the run measured something other
    # than what it claims -- unwritten-extent conversion, a layout pass
    # bleeding into a point, an observation window misaligned with fio -- and
    # no scaling shape derived from it means anything. Printed FIRST and
    # loudly, because the alternative is a plausible-looking curve.
    # ---------------------------------------------------------------
    bad = []
    for (c, l, nj), rows in sorted(pts.items()):
        if c != "direct":
            continue
        app = med([r["app"] for r in rows])
        dev = med([r["dev_mbps"] for r in rows])
        if dev > 0 and abs(app / dev - 1.0) > 0.15:
            bad.append((l, nj, app / dev))

    print("\n" + "=" * 78)
    if bad:
        print("CONTROL FAILED -- %d direct points disagree with the device" % len(bad))
        print("=" * 78)
        for l, nj, r in bad:
            print("  direct/%-7s %2d jobs: app/dev = %.2f" % (l, nj, r))
        print("")
        print("  With O_DIRECT there is no cache in the path, so these must be")
        print("  1.00. They are not, which makes this a MEASUREMENT problem and")
        print("  not a kernel result. NOTHING BELOW MAY BE QUOTED.")
        print("")
        print("  Usual causes, in the order worth checking:")
        print("    - the layout pass did not run, so writes still convert")
        print("      unwritten extents and XFS journals work fio never sees")
        print("    - a point overlaps the previous point's writeback")
        print("    - iostat is watching a different device than fio writes to")
    else:
        print("CONTROL OK -- every direct point agrees with the device within 15%")
        print("=" * 78)
        print("  O_DIRECT has no cache in the path, so app/dev == 1.00 is the")
        print("  expectation this arm exists to check. It holds, so the buffered")
        print("  arms' divergence is a property of the page cache rather than")
        print("  of the measurement.")

    print("\n" + "=" * 78)
    print("SCALING SHAPE  (device-side throughput, first jobs -> last jobs)")
    print("=" * 78)
    for (c, l), series in sorted(curves.items()):
        series = sorted(set(series))
        if len(series) < 2:
            continue
        lo, hi = series[0], series[-1]
        gain = hi[1] / lo[1] if lo[1] > 0 else 0.0
        peak = max(series, key=lambda s: s[1])
        print("  %-9s %-7s  %2d jobs %7.1f -> %2d jobs %7.1f MB/s   %.2fx"
              "   (peak %.1f at %d jobs)"
              % (c, l, lo[0], lo[1], hi[0], hi[1], gain, peak[1], peak[0]))

    print("""
READING IT
  For each contract, compare `shared` against `perjob`. i_rwsem is PER-INODE,
  so it cannot be contended when every thread owns its own file.

    shared plateaus, perjob scales   -> the inode lock is implicated
    both plateau at the same point   -> something else is; the cache is a
                                        candidate, and so is the device
    both scale                       -> there was no plateau to explain

  In every case the shape is as far as the throughput curve can take you.
  WHICH lock is a question for the profiles:

      grep -iE 'rwsem|down_write|xas_|__filemap|queued_spin' \\
           %s/*_profile.txt

  Do not write the word XArray into any claim that this report alone supports.
""" % d)


if __name__ == "__main__":
    main()
