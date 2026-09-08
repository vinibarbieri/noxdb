#!/usr/bin/env python3
"""
fio_report.py - summarise a fio sweep, and flag the artifacts that make a
summary a lie.

    ./tools/fio_report.py /mnt/nvme/results/20260820-032106
    ./tools/fio_report.py RESULT_DIR --anchor randwrite4k:2:410.0

Reporting a median alone hides exactly the failure this project already hit
twice: samples that drift with execution time rather than scatter around a
value. So every point is printed with its repetitions in EXECUTION order, and
two checks run automatically:

  DRIFT     Are the samples monotonic in execution order? A monotone series is
            a transient being averaged into a "result". Reported whenever the
            first sample is also the extreme one, which is what a warm-up
            transient looks like.

  ANCHOR    --anchor NAME:QD:MBPS compares a point against the same point from
            an earlier run. If the anchor does not reproduce, NOTHING in this
            run may be compared against that earlier run. This is the check
            that caught the 1M sequential shape being 22% low on 2026-08-20.

Also prints the active-benchmarking observation (aqu-sz, %util) when the sweep
recorded one, since a bandwidth number without the device's own view of its
queue is only half a measurement.
"""
import json
import os
import re
import sys
import glob
import statistics


def load(result_dir):
    pts = {}
    for f in glob.glob(os.path.join(result_dir, "*_rep*.json")):
        m = re.match(r"(.+)_qd(\d+)_rep(\d+)\.json", os.path.basename(f))
        if not m:
            continue
        try:
            j = json.load(open(f))
        except Exception as e:
            print("  ! unreadable: %s (%s)" % (os.path.basename(f), e))
            continue
        w = j["jobs"][0]["write"]
        pts.setdefault((m.group(1), int(m.group(2))), []).append({
            "rep": int(m.group(3)),
            "mbps": w["bw_bytes"] / 1e6,
            "iops": w["iops"],
            "p99": w["clat_ns"]["percentile"].get("99.000000", 0) / 1000.0,
            "mean": w["clat_ns"]["mean"] / 1000.0,
            "iostat": iostat_summary(result_dir,
                                     "%s_qd%s_rep%s" % (m.group(1), m.group(2), m.group(3))),
        })
    return pts


def iostat_summary(result_dir, tag):
    """Median aqu-sz and %util from the parallel iostat capture, if present."""
    path = os.path.join(result_dir, tag + ".iostat")
    if not os.path.exists(path):
        return None
    aq, ut, ai, ui = [], [], None, None
    for line in open(path):
        f = line.split()
        if not f:
            continue
        if f[0] == "Device":
            ai = f.index("aqu-sz") if "aqu-sz" in f else None
            ui = f.index("%util") if "%util" in f else None
        elif ai is not None and ui is not None and len(f) > max(ai, ui):
            try:
                aq.append(float(f[ai]))
                ut.append(float(f[ui]))
            except ValueError:
                pass
    if not aq:
        return None
    return (statistics.median(aq), statistics.median(ut))


def report(result_dir, anchors):
    pts = load(result_dir)
    if not pts:
        print("no fio json found in %s" % result_dir)
        return 1
    print("=== %s ===" % result_dir)
    warnings = []

    for key in sorted(pts):
        name, qd = key
        v = sorted(pts[key], key=lambda x: x["rep"])
        bw = [x["mbps"] for x in v]
        med = statistics.median(bw)
        spread = (max(bw) - min(bw)) / med * 100 if med else 0

        print("\n--- %s QD%d ---  median %.1f MB/s   spread %.1f%%  (n=%d)"
              % (name, qd, med, spread, len(v)))
        for x in v:
            obs = ""
            if x["iostat"]:
                obs = "   aqu-sz=%.2f %%util=%.1f" % x["iostat"]
            print("   rep%-2d %8.1f MB/s %8.0f iops  p99=%9.1f us  mean=%8.1f us%s"
                  % (x["rep"], x["mbps"], x["iops"], x["p99"], x["mean"], obs))

        # DRIFT: a warm-up transient makes the FIRST sample the extreme one and
        # the rest cluster. Distinguishing that from honest scatter is the whole
        # point -- with it flagged you can drop rep1 and say so; without it you
        # publish a median that no repetition actually produced.
        if len(v) >= 3:
            rest = bw[1:]
            rest_med = statistics.median(rest)
            rest_spread = (max(rest) - min(rest)) / rest_med * 100 if rest_med else 0
            if spread > 5 and rest_spread < spread / 3:
                warnings.append(
                    "%s QD%d: rep1 (%.1f) is an outlier; reps 2..%d give "
                    "%.1f MB/s at %.1f%% spread. Warm-up transient -- drop rep1 "
                    "and SAY SO, or re-run with a longer WARMUP."
                    % (name, qd, bw[0], len(v), rest_med, rest_spread))
            if rest_spread > 5 and (rest == sorted(rest) or rest == sorted(rest, reverse=True)):
                warnings.append(
                    "%s QD%d: reps 2..%d are MONOTONIC (%.1f -> %.1f). The point "
                    "had not converged; this is a transient, not a value."
                    % (name, qd, len(v), rest[0], rest[-1]))

        for a_name, a_qd, a_val in anchors:
            if a_name == name and a_qd == qd:
                rest_med = statistics.median(bw[1:]) if len(bw) > 1 else med
                delta = (rest_med - a_val) / a_val * 100
                verdict = "REPRODUCES" if abs(delta) <= 2 else "DOES NOT REPRODUCE"
                print("   anchor: %.1f expected, %.1f measured (%+.1f%%) -> %s"
                      % (a_val, rest_med, delta, verdict))
                if abs(delta) > 2:
                    warnings.append(
                        "%s QD%d anchor off by %+.1f%%. Nothing in this run may "
                        "be compared against the run that produced %.1f MB/s."
                        % (name, qd, delta, a_val))

    if warnings:
        print("\n=== WARNINGS ===")
        for w in warnings:
            print("  ! " + w)
    else:
        print("\nno drift or anchor warnings.")
    return 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    anchors = []
    for a in sys.argv[1:]:
        if a.startswith("--anchor="):
            n, q, v = a.split("=", 1)[1].split(":")
            anchors.append((n, int(q), float(v)))
    if not args:
        print(__doc__)
        sys.exit(2)
    sys.exit(report(args[0], anchors))
