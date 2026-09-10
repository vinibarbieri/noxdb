#!/usr/bin/env python3
"""
plot_ceiling.py - render the PERFORMANCE.md §3.1 device ceiling as an SVG.

    ./tools/plot_ceiling.py                      # defaults below
    ./tools/plot_ceiling.py CSV OUT.svg

Input is the per-repetition CSV under docs/figures/, not the medians in
PERFORMANCE.md: the whiskers need each repetition's min and max, and the
Markdown table records spread only as a percentage.

Two panels share one x-axis instead of one panel with two y-axes. Throughput
and p99 have different units and a 130x difference in range; a second y-scale
would let the reader compare slopes that mean nothing against each other.

Throughput stays linear from zero, so the QD1 -> QD2 knee reads at its real
size. p99 goes on a log scale, because on a linear one QD1 through QD4 would
sit flat against the baseline.

Standard library only, so it runs on the dev machine and the bench box alike.
"""
import csv
import math
import os
import statistics
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CSV_IN = os.path.join(ROOT, "docs", "figures", "ceiling-4k-randwrite.csv")
SVG_OUT = os.path.join(ROOT, "docs", "figures", "ceiling-4k-randwrite.svg")

# One fixed light look with an opaque background: GitHub's image proxy gives
# no reliable way to follow the viewer's theme, and a transparent SVG would
# put dark text on a dark page.
SURFACE = "#ffffff"
INK = "#0b0b0b"
INK_2 = "#52514e"
INK_3 = "#76756f"
GRID = "#e6e5e0"
SERIES = "#2a78d6"
FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif"

W = 760
LEFT, RIGHT = 76, 28
PAD = 30                                  # half-slot of air inside the plot edges
QDS = [1, 2, 4, 8, 16, 32, 64]

CAPTION = (
    "Method: fio 3.36, ioengine=libaio, direct=1, rw=randwrite, bs=4k, 16 GiB "
    "file on XFS at /mnt/nvme. 5 repetitions × 120 s per point (10 s ramp), "
    "drop_caches between points, run after M2 preconditioning (80% of free "
    "space written twice with 1 MiB O_DIRECT sequential writes). Run "
    "20260820-032106. Marker = median; whiskers = min–max over repetitions. "
    "Most ranges are narrower than the marker, so the spread rows, "
    "(max − min) / median, carry the number. p99 is read from fio's "
    "bucketed clat histogram, so repetitions often report identical values. "
    "Requested iodepth is not the device-observed queue depth: at QD2, iostat "
    "reported aqu-sz 1.09 in a later re-measurement (PERFORMANCE.md §3.1)."
)
FOOTNOTE = (
    "† QD1 comes from run rerun-qd1 (same parameters, shuffled point order). "
    "Its repetition 1 was a warm-up transient and is excluded, so n = 4. The "
    "main run's QD1 climbed monotonically and was withheld; QD2 re-measured "
    "in rerun-qd1 at −0.7% of the value shown. Data: "
    "docs/figures/ceiling-4k-randwrite.csv. Table: PERFORMANCE.md §3.1."
)


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=12, fill=INK_2, anchor="start", weight="normal"):
    return ('<text x="%.1f" y="%.1f" font-size="%d" fill="%s" text-anchor="%s" '
            'font-weight="%s">%s</text>' % (x, y, size, fill, anchor, weight, esc(s)))


def load(path):
    pts = {}
    with open(path) as f:
        rows = csv.DictReader(line for line in f if not line.startswith("#"))
        for r in rows:
            if r["plotted"] != "1":
                continue
            pts.setdefault(int(r["qd"]), []).append(
                (float(r["mb_s"]), float(r["p99_us"])))
    missing = [q for q in QDS if q not in pts]
    if missing:
        sys.exit("plot_ceiling: no plotted rows for QD %s" % missing)
    out = []
    for q in QDS:
        mb = [p[0] for p in pts[q]]
        p99 = [p[1] for p in pts[q]]
        out.append({
            "qd": q, "n": len(mb),
            "mb": (statistics.median(mb), min(mb), max(mb)),
            "p99": (statistics.median(p99), min(p99), max(p99)),
        })
    return out


def spread(t):
    med, lo, hi = t
    return (hi - lo) / med * 100.0


def xpos(i):
    return LEFT + PAD + i * (W - LEFT - RIGHT - 2 * PAD) / (len(QDS) - 1)


def panel(pts, key, top, height, yscale, ticks, tick_fmt, title, label_idx):
    """One panel: title, gridlines, whiskers, 2px line, ringed markers."""
    bottom = top + height
    y = lambda v: bottom - yscale(v) * height
    g = [text(LEFT - 8, top - 14, title, size=13, fill=INK, weight="600")]

    for t in ticks:
        yy = y(t)
        g.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="%s" '
                 'stroke-width="1"/>' % (LEFT, W - RIGHT, yy, yy, GRID))
        g.append(text(LEFT - 8, yy + 4, tick_fmt(t), size=11, fill=INK_3,
                      anchor="end"))

    # Whiskers go underneath the markers: a range narrower than the marker is
    # hidden rather than drawn as a misleading stub, and the caption says so.
    for i, p in enumerate(pts):
        _, lo, hi = p[key]
        x = xpos(i)
        g.append('<g stroke="%s" stroke-width="1.5" stroke-linecap="round">'
                 '<line x1="%.1f" x2="%.1f" y1="%.1f" y2="%.1f"/>'
                 '<line x1="%.1f" x2="%.1f" y1="%.1f" y2="%.1f"/>'
                 '<line x1="%.1f" x2="%.1f" y1="%.1f" y2="%.1f"/></g>'
                 % (INK_2, x, x, y(lo), y(hi),
                    x - 6, x + 6, y(lo), y(lo), x - 6, x + 6, y(hi), y(hi)))

    path = " ".join("%s%.1f,%.1f" % ("M" if i == 0 else "L", xpos(i), y(p[key][0]))
                    for i, p in enumerate(pts))
    g.append('<path d="%s" fill="none" stroke="%s" stroke-width="2" '
             'stroke-linejoin="round" stroke-linecap="round"/>' % (path, SERIES))

    for i, p in enumerate(pts):
        g.append('<circle cx="%.1f" cy="%.1f" r="4" fill="%s" stroke="%s" '
                 'stroke-width="2"/>' % (xpos(i), y(p[key][0]), SERIES, SURFACE))

    # Selective direct labels: only the points the reading hangs on.
    # Each label sits on the side of its point the line does not pass through;
    # the last point's label is end-anchored so it cannot run off the canvas.
    for i, fmt, dy in label_idx:
        med = pts[i][key][0]
        last = i == len(pts) - 1
        g.append(text(xpos(i) + (-10 if last else 10), y(med) + dy, fmt(med),
                      size=11, fill=INK, anchor="end" if last else "start"))
    return g, bottom


def spread_row(pts, key, yy, label):
    g = [text(LEFT - 8, yy, label, size=11, fill=INK_3, anchor="end")]
    for i, p in enumerate(pts):
        g.append(text(xpos(i), yy, "%.1f%%" % spread(p[key]), size=11,
                      fill=INK_2, anchor="middle"))
    return g


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else CSV_IN
    dst = sys.argv[2] if len(sys.argv) > 2 else SVG_OUT
    pts = load(src)

    body = []
    body.append(text(LEFT - 8, 30, "WD SN530 device ceiling: 4 KiB random write "
                     "vs queue depth", size=16, fill=INK, weight="600"))
    body.append(text(LEFT - 8, 50, "Raw O_DIRECT through fio, no NoxDB code in "
                     "the path. Same x-axis in both panels.", size=12, fill=INK_2))

    # Panel A: throughput, linear from zero.
    mb_max = 450.0
    a, a_bottom = panel(
        pts, "mb", top=96, height=190,
        yscale=lambda v: v / mb_max,
        ticks=[0, 100, 200, 300, 400],
        tick_fmt=lambda t: "%d" % t,
        title="Throughput (MB/s, median)",
        label_idx=[(0, lambda v: "%.1f" % v, 16),
                   (1, lambda v: "%.1f" % v, 16),
                   (6, lambda v: "%.1f" % v, 16)])
    body += a
    body += spread_row(pts, "mb", a_bottom + 20, "spread")

    # Panel B: p99, log scale across 10 us .. 3 ms.
    lo_d, hi_d = math.log10(10), math.log10(3000)
    b, b_bottom = panel(
        pts, "p99", top=a_bottom + 72, height=230,
        yscale=lambda v: (math.log10(v) - lo_d) / (hi_d - lo_d),
        ticks=[10, 30, 100, 300, 1000, 3000],
        tick_fmt=lambda t: "{:,}".format(t),
        title="p99 completion latency (µs, median, log scale)",
        label_idx=[(0, lambda v: "%.1f µs" % v, 16),
                   (6, lambda v: "{:,.0f} µs".format(v), -10)])
    body += b

    yy = b_bottom + 20
    for i, q in enumerate(QDS):
        body.append(text(xpos(i), yy, "%d†" % q if q == 1 else str(q),
                         size=12, fill=INK, anchor="middle"))
    body += spread_row(pts, "p99", yy + 20, "spread")
    body.append(text((LEFT + W - RIGHT) / 2, yy + 44,
                     "fio --iodepth (requested queue depth), log₂ spacing",
                     size=12, fill=INK_2, anchor="middle"))

    yy += 76
    body.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="%s" '
                'stroke-width="1"/>' % (LEFT - 8, W - RIGHT, yy - 16, yy - 16, GRID))
    for block in (CAPTION, FOOTNOTE):
        for line in textwrap.wrap(block, 112):
            body.append(text(LEFT - 8, yy, line, size=11, fill=INK_2))
            yy += 15
        yy += 6

    h = int(yy + 8)
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
           'viewBox="0 0 %d %d" font-family="%s" role="img" '
           'aria-labelledby="t d">' % (W, h, W, h, FONT),
           '<title id="t">WD SN530 4 KiB random write: throughput and p99 vs '
           'queue depth</title>',
           '<desc id="d">Throughput rises from %.1f MB/s at QD1 to %.1f at QD2 '
           'and stays near %.1f through QD64, while p99 grows from %.1f us to '
           '%.0f us.</desc>' % (pts[0]["mb"][0], pts[1]["mb"][0],
                                pts[6]["mb"][0], pts[0]["p99"][0],
                                pts[6]["p99"][0]),
           '<rect width="100%%" height="100%%" fill="%s"/>' % SURFACE]
    svg += body
    svg.append("</svg>\n")

    with open(dst, "w") as f:
        f.write("\n".join(svg))
    for p in pts:
        print("QD%-2d n=%d  %6.1f MB/s  spread %.1f%%  p99 %7.1f us  spread %.1f%%"
              % (p["qd"], p["n"], p["mb"][0], spread(p["mb"]),
                 p["p99"][0], spread(p["p99"])))
    print("wrote", os.path.relpath(dst, ROOT))


if __name__ == "__main__":
    main()
