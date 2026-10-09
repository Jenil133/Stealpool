#!/usr/bin/env python3
"""Draw the scaling charts in docs/assets/ from the benchmark results in bench/results/.

Usage:
    scripts/plot_scaling.py [--results DIR] [--out DIR] [--suffix SUFFIX]

Reads the eight result files {macos,linux-gcc}-{parallel-sort,tree-default,tree-fine,
task-overhead}<SUFFIX>.json (SUFFIX defaults to "", the first pass; --suffix=-run2 selects
the second) and writes:

  speedup-<platform>-<workload>.svg   speedup over the same pool on one thread, per thread
                                      count, work stealing vs shared queue (8 charts)
  ratio-8-16-threads.svg              shared_queue time / work_stealing time at 8, 10, 12
                                      and 16 threads for every workload and platform

Every value is the median wall-clock time per run, taken exactly as scripts/bench_report.py
takes it (Google Benchmark's median aggregate), so the charts match the .md tables next to
the JSON files. The SVGs have a transparent background and use mid-tone colors that keep
their contrast on both light and dark pages. Each data mark carries data-* attributes and a
<title> with its exact value.

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path
from xml.sax.saxutils import escape

sys.dont_write_bytecode = True  # keep scripts/ free of __pycache__
sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_report import NAME_RE, collect, load, public_context  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# (file label, benchmark name in the JSON, chart title)
WORKLOADS = [
    ("parallel-sort", "parallel_sort", "Parallel merge sort"),
    ("tree-default", "tree_reduce", "Unbalanced tree reduction, default"),
    ("tree-fine", "tree_reduce", "Unbalanced tree reduction, fine-grained"),
    ("task-overhead", "task_overhead", "Task overhead"),
]
# (output name, file prefix, display name)
PLATFORMS = [("macos", "macos", "macOS"), ("linux", "linux-gcc", "Linux")]
THREADS = [1, 2, 4, 6, 8, 10, 12, 16]
RATIO_THREADS = [8, 10, 12, 16]
POOLS = [("work_stealing", "work stealing"), ("shared_queue", "shared queue")]

# Mid-tone colors chosen to stay readable on white and on near-black pages.
INK = "#6e7781"  # text: 4.5:1 on #ffffff, 4.2:1 on #0d1117
GRID = "#8b949e"
POOL_COLORS = {"work_stealing": "#3987e5", "shared_queue": "#e5622a"}
THREAD_COLORS = {8: "#86b6ef", 10: "#5598e7", 12: "#2a78d6", 16: "#1c5cab"}
FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif"


class Series:
    """Median milliseconds per (pool, threads) for one workload on one platform."""

    def __init__(self, path: Path, benchmark: str) -> None:
        contexts, rows = load([path])
        self.path = path
        self.context = public_context(contexts[0])
        times = collect(rows, "median")
        self.ms = {(p, t): v for (w, p, t), v in times.items() if w == benchmark}
        self.repetitions = self._count_repetitions(rows, benchmark)
        missing = [(p, t) for p, _ in POOLS for t in THREADS if (p, t) not in self.ms]
        if missing:
            sys.exit(f"{path}: no median for {missing}")

    @staticmethod
    def _count_repetitions(rows: list[dict], benchmark: str) -> int:
        counts: dict[str, int] = {}
        for row in rows:
            match = NAME_RE.match(row.get("run_name", row.get("name", "")))
            if match and match["workload"] == benchmark and row.get("run_type") != "aggregate":
                counts[match.group(0)] = counts.get(match.group(0), 0) + 1
        values = set(counts.values())
        return values.pop() if len(values) == 1 else 0

    def speedup(self, pool: str, threads: int) -> float:
        return self.ms[(pool, 1)] / self.ms[(pool, threads)]

    def ratio(self, threads: int) -> float:
        return self.ms[("shared_queue", threads)] / self.ms[("work_stealing", threads)]

    def workload_note(self, label: str) -> str:
        ctx = self.context
        if label == "parallel-sort":
            return f"{int(ctx['sort_size']):,} ints"
        if label.startswith("tree"):
            return (f"{int(ctx['tree_nodes']):,} nodes, cutoff {ctx['tree_cutoff']}, "
                    f"{ctx['tree_work']} rounds per node")
        return f"{(1 << int(ctx['spawn_depth'])) - 1:,} empty tasks"


def threads_text(threads: int) -> str:
    return f"{threads} thread" if threads == 1 else f"{threads} threads"


def num(value: float) -> str:
    """Shortest plain form for a coordinate or tick value."""
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return "0" if text == "-0" else text


def svg_document(width: int, height: int, title: str, desc: str, body: list[str]) -> str:
    style = (
        f"text{{font-family:{FONT};fill:{INK}}}"
        ".title{font-size:16px;font-weight:600}"
        ".sub{font-size:12px}"
        ".tick{font-size:11px;font-variant-numeric:tabular-nums}"
        ".axis{font-size:12px}"
        ".label{font-size:12px;font-weight:600}"
        ".value{font-size:11px;font-variant-numeric:tabular-nums}"
        f".grid{{stroke:{GRID};stroke-opacity:.25;stroke-width:1}}"
        f".base{{stroke:{GRID};stroke-opacity:.7;stroke-width:1}}"
        f".mark8{{stroke:{GRID};stroke-opacity:.8;stroke-width:1;stroke-dasharray:4 4}}"
    )
    head = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
        f'<title id="title">{escape(title)}</title>',
        f'<desc id="desc">{escape(desc)}</desc>',
        f"<style>{style}</style>",
    ]
    return "\n".join(head + body + ["</svg>"]) + "\n"


def nice_step(top: float, max_intervals: int) -> float:
    for step in (0.25, 0.5, 1, 2, 2.5, 5, 10, 20, 25, 50, 100):
        if math.ceil(top / step) <= max_intervals:
            return step
    return 10 ** math.ceil(math.log10(top))


def speedup_chart(series: Series, platform: str, label: str, title: str) -> str:
    width, height = 640, 400
    left, right, top, bottom = 56, width - 24, 96, height - 52
    peak = max(series.speedup(p, t) for p, _ in POOLS for t in THREADS)
    step = nice_step(peak * 1.05, 6)
    y_max = step * math.ceil(peak * 1.05 / step)

    def x(threads: float) -> float:
        return left + (right - left) * threads / THREADS[-1]

    def y(value: float) -> float:
        return bottom - (bottom - top) * value / y_max

    reps = f"median of {series.repetitions} repetitions" if series.repetitions else "median"
    body = [
        f'<text class="title" x="{left}" y="28">{escape(title)} · {platform}</text>',
        f'<text class="sub" x="{left}" y="48">{escape(series.workload_note(label))} · {reps}</text>',
    ]
    # Legend.
    lx = left
    for pool, name in POOLS:
        color = POOL_COLORS[pool]
        dash = ' stroke-dasharray="6 4"' if pool == "shared_queue" else ""
        body.append(f'<line x1="{lx}" y1="70" x2="{lx + 24}" y2="70" stroke="{color}" stroke-width="2"{dash}/>')
        body.append(marker(pool, lx + 12, 70, ""))
        body.append(f'<text class="axis" x="{lx + 32}" y="74">{name}</text>')
        lx += 32 + 8 * len(name) + 24
    # Grid, ticks and axes.
    tick = 0.0
    while tick <= y_max + 1e-9:
        cls = "base" if tick == 0 else "grid"
        body.append(f'<line class="{cls}" x1="{left}" y1="{num(y(tick))}" x2="{right}" y2="{num(y(tick))}"/>')
        body.append(f'<text class="tick" x="{left - 8}" y="{num(y(tick) + 4)}" text-anchor="end">{num(tick)}</text>')
        tick += step
    for t in THREADS:
        body.append(f'<line class="grid" x1="{num(x(t))}" y1="{top}" x2="{num(x(t))}" y2="{bottom}"/>')
        body.append(f'<text class="tick" x="{num(x(t))}" y="{bottom + 18}" text-anchor="middle">{t}</text>')
    body.append(f'<line class="base" x1="{left}" y1="{top}" x2="{left}" y2="{bottom}"/>')
    body.append(f'<text class="axis" x="{(left + right) / 2:.0f}" y="{height - 12}" text-anchor="middle">threads</text>')
    body.append(f'<text class="axis" transform="translate(16 {(top + bottom) / 2:.0f}) rotate(-90)" '
                f'text-anchor="middle">speedup vs 1 thread (×)</text>')
    # Marker at 8 threads.
    body.append(f'<line class="mark8" x1="{num(x(8))}" y1="{top - 4}" x2="{num(x(8))}" y2="{bottom}"/>')
    body.append(f'<text class="axis" x="{num(x(8) + 6)}" y="{top + 8}">8 threads</text>')
    # Lines and points.
    for pool, name in POOLS:
        color = POOL_COLORS[pool]
        dash = ' stroke-dasharray="6 4"' if pool == "shared_queue" else ""
        points = " ".join(f"{num(x(t))},{num(y(series.speedup(pool, t)))}" for t in THREADS)
        body.append(f'<polyline fill="none" stroke="{color}" stroke-width="2" stroke-linejoin="round"{dash} points="{points}"/>')
        for t in THREADS:
            s, ms = series.speedup(pool, t), series.ms[(pool, t)]
            tip = f"{name}, {threads_text(t)}: {ms:.4g} ms median, {s:.2f}x"
            attrs = f'data-pool="{pool}" data-threads="{t}" data-ms="{ms:.9g}" data-speedup="{s:.9g}"'
            body.append(marker(pool, x(t), y(s), attrs, tip))
    # Values at 8 threads, placed on the side away from the incoming line.
    labels = []
    for pool, _ in POOLS:
        s8 = series.speedup(pool, 8)
        above = series.speedup(pool, 6) <= s8 or y(s8) + 18 > bottom - 6
        labels.append([y(s8) + (-10 if above else 18), f"{s8:.2f}×", pool])
    labels.sort()
    if labels[1][0] - labels[0][0] < 14:
        mid = (labels[0][0] + labels[1][0]) / 2
        labels[0][0], labels[1][0] = mid - 7, mid + 7
    for ly, text, pool in labels:
        body.append(f'<text class="label" x="{num(x(8) - 8)}" y="{num(ly)}" text-anchor="end" '
                    f'style="fill:{POOL_COLORS[pool]}">{text}</text>')
    desc = (f"{title} on {platform}: speedup at "
            + ", ".join(f"{threads_text(t)} work stealing {series.speedup('work_stealing', t):.2f}x, "
                        f"shared queue {series.speedup('shared_queue', t):.2f}x" for t in THREADS) + ".")
    return svg_document(width, height, f"{title} · {platform}", desc, body)


def marker(pool: str, cx: float, cy: float, attrs: str, tip: str = "") -> str:
    color = POOL_COLORS[pool]
    inner = f"<title>{escape(tip)}</title>" if tip else ""
    if pool == "shared_queue":
        shape = f'<rect x="{num(cx - 4)}" y="{num(cy - 4)}" width="8" height="8" fill="{color}" {attrs}'
    else:
        shape = f'<circle cx="{num(cx)}" cy="{num(cy)}" r="4.5" fill="{color}" {attrs}'
    shape = shape.rstrip()
    return f"{shape}>{inner}</{'rect' if pool == 'shared_queue' else 'circle'}>"


def ratio_chart(data: dict[tuple[str, str], Series]) -> str:
    width = 960
    left, right = 150, width - 56
    bar, gap, platform_gap, header, group_gap = 11, 3, 10, 22, 18
    block = len(RATIO_THREADS) * (bar + gap) - gap
    top = 100
    height = top + len(WORKLOADS) * (header + 2 * block + platform_gap + group_gap) - group_gap + 52

    ratios = [s.ratio(t) for s in data.values() for t in RATIO_THREADS]
    ticks = [v * m for m in (0.01, 0.1, 1, 10, 100, 1000) for v in (1, 2, 5)]
    lo = min(min(ratios), 1) / 1.25  # room for bars below 1
    hi = min(v for v in ticks if v >= max(ratios) * 1.15)
    ticks = [v for v in ticks if lo <= v <= hi]

    def x(value: float) -> float:
        return left + (right - left) * (math.log10(value) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))

    plot_bottom = height - 52
    body = [
        f'<text class="title" x="24" y="28">Shared-queue time / work-stealing time at '
        f'{RATIO_THREADS[0]}–{RATIO_THREADS[-1]} threads</text>',
        '<text class="sub" x="24" y="48">Above 1: work stealing is faster. Median wall-clock time per '
        'run for each pool; log scale.</text>',
        f'<text class="axis" x="24" y="76">threads</text>',
    ]
    lx = 84
    for t in RATIO_THREADS:
        body.append(f'<rect x="{lx}" y="66" width="14" height="11" rx="2" fill="{THREAD_COLORS[t]}"/>')
        body.append(f'<text class="axis" x="{lx + 20}" y="76">{t}</text>')
        lx += 56
    for v in ticks:
        body.append(f'<text class="tick" x="{num(x(v))}" y="{plot_bottom + 18}" text-anchor="middle">{num(v)}</text>')
    body.append(f'<text class="axis" x="{(left + right) / 2:.0f}" y="{height - 12}" text-anchor="middle">'
                "shared_queue time / work_stealing time (×, log scale)</text>")

    cy = top
    for label, _, title in WORKLOADS:
        body.append(f'<text class="label" x="24" y="{cy + 12}">{escape(title)}</text>')
        cy += header
        for name, _, platform in PLATFORMS:
            series = data[(name, label)]
            body.append(f'<text class="axis" x="40" y="{num(cy + block / 2 + 4)}">{platform}</text>')
            for v in ticks:
                cls = "base" if v == 1 else "grid"
                body.append(f'<line class="{cls}" x1="{num(x(v))}" y1="{cy - 4}" x2="{num(x(v))}" y2="{cy + block + 4}"/>')
            for t in RATIO_THREADS:
                r = series.ratio(t)
                x0, x1 = sorted((x(1), x(r)))
                ws, sq = series.ms[("work_stealing", t)], series.ms[("shared_queue", t)]
                tip = (f"{title}, {platform}, {threads_text(t)}: shared queue {sq:.4g} ms / "
                       f"work stealing {ws:.4g} ms = {r:.2f}")
                body.append(f'<text class="tick" x="{left - 10}" y="{num(cy + bar - 2)}" text-anchor="end">{t}</text>')
                body.append(
                    f'<rect x="{num(x0)}" y="{num(cy)}" width="{num(max(x1 - x0, 1))}" height="{bar}" rx="2" '
                    f'fill="{THREAD_COLORS[t]}" data-workload="{label}" data-platform="{name}" '
                    f'data-threads="{t}" data-ratio="{r:.9g}"><title>{escape(tip)}</title></rect>')
                body.append(f'<text class="value" x="{num(x1 + 5)}" y="{num(cy + bar - 2)}">{r:.2f}</text>')
                cy += bar + gap
            cy += platform_gap - gap
        cy += group_gap - platform_gap
    desc = "; ".join(
        f"{title}, {platform}: " + ", ".join(f"{threads_text(t)} {data[(name, label)].ratio(t):.2f}" for t in RATIO_THREADS)
        for label, _, title in WORKLOADS for name, _, platform in PLATFORMS)
    return svg_document(width, height, "Shared-queue / work-stealing time ratio", desc + ".", body)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--results", type=Path, default=ROOT / "bench" / "results", help="result JSON directory")
    parser.add_argument("--out", type=Path, default=ROOT / "docs" / "assets", help="output directory for the SVGs")
    parser.add_argument("--suffix", default="", help="file name suffix of the pass to plot, e.g. --suffix=-run2")
    args = parser.parse_args()

    data: dict[tuple[str, str], Series] = {}
    for name, prefix, _ in PLATFORMS:
        for label, benchmark, _ in WORKLOADS:
            path = args.results / f"{prefix}-{label}{args.suffix}.json"
            if not path.is_file():
                sys.exit(f"missing result file: {path}")
            data[(name, label)] = Series(path, benchmark)

    args.out.mkdir(parents=True, exist_ok=True)
    written = []
    for name, _, platform in PLATFORMS:
        for label, _, title in WORKLOADS:
            target = args.out / f"speedup-{name}-{label}.svg"
            target.write_text(speedup_chart(data[(name, label)], platform, label, title))
            written.append(target)
    target = args.out / "ratio-8-16-threads.svg"
    target.write_text(ratio_chart(data))
    written.append(target)
    for path in written:
        print(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path)


if __name__ == "__main__":
    main()
