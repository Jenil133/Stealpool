#!/usr/bin/env python3
"""Turn stealpool_bench JSON output into markdown speedup tables.

Usage:
    scripts/bench_report.py RESULTS.json [MORE.json ...] [--stat median|mean|min] [-o OUT.md]
    scripts/bench_report.py RAW.json --write-json RESULTS.json [-o OUT.md]

For each workload (parallel_sort, tree_reduce, task_overhead) it prints one table with a
row per thread count and, for every pool, the wall-clock time and the speedup over the
same pool with one thread. The last column compares the pools at equal thread count
(shared-queue time divided by work-stealing time; above 1 means work stealing is faster).

Statistic: with --benchmark_repetitions the JSON holds per-repetition rows and aggregate
rows. "median" and "mean" use Google Benchmark's aggregates when present and otherwise
compute them from the repetitions; "min" always uses the fastest repetition.

Several input files are combined into one set of tables, so they must hold disjoint
benchmarks from one run on one machine (for example one file per workload, written by the
same build with the same options). A benchmark (workload, pool, thread count) that appears
in more than one file is an error: mixing runs would silently pick one of them and could
divide by a one-thread baseline from a different run. Report separate runs separately.

Result files are meant to be published, so the machine-specific and dated fields Google
Benchmark puts in the JSON context (run date, host name, executable path) are never printed.
--write-json PATH copies a single input file to PATH without them; the rest of the context
(CPU count, caches, load average, compiler, build type, library version, workload options)
and every benchmark row are kept unchanged.

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

NAME_RE = re.compile(r"^(?P<workload>[^/]+)/(?P<pool>[^/]+)/threads:(?P<threads>\d+)")

POOL_ORDER = ["work_stealing", "shared_queue"]
WORKLOAD_TITLES = {
    "parallel_sort": "Parallel merge sort",
    "tree_reduce": "Unbalanced tree reduction",
    "task_overhead": "Task overhead (empty fork-join tree)",
}
TO_MS = {"ns": 1e-6, "us": 1e-3, "ms": 1.0, "s": 1e3}
# Context fields that identify the machine or the run date; dropped from published output.
PRIVATE_CONTEXT = ("date", "host_name", "executable")


def public_context(ctx: dict) -> dict:
    return {k: v for k, v in ctx.items() if k not in PRIVATE_CONTEXT}


def write_public_json(source: Path, target: Path) -> None:
    with source.open() as f:
        data = json.load(f)
    data["context"] = public_context(data.get("context", {}))
    target.write_text(json.dumps(data, indent=2) + "\n")


def load(paths: list[Path]) -> tuple[list[dict], list[dict]]:
    contexts, rows = [], []
    owner: dict[tuple[str, str, int], Path] = {}
    for path in paths:
        with path.open() as f:
            data = json.load(f)
        contexts.append(public_context(data.get("context", {})))
        for row in data.get("benchmarks", []):
            match = NAME_RE.match(row.get("run_name", row.get("name", "")))
            if match:
                key = (match["workload"], match["pool"], int(match["threads"]))
                if owner.setdefault(key, path) != path:
                    sys.exit(
                        f"{match.group(0)} appears in both {owner[key]} and {path}; "
                        "combine only disjoint benchmarks from one run (report other runs separately)"
                    )
            rows.append(row)
    return contexts, rows


def collect(rows: list[dict], stat: str) -> dict[tuple[str, str, int], float]:
    """Returns {(workload, pool, threads): milliseconds}."""
    samples: dict[tuple[str, str, int], list[float]] = defaultdict(list)
    aggregates: dict[tuple[str, str, int], float] = {}
    errors = []
    for row in rows:
        match = NAME_RE.match(row.get("run_name", row.get("name", "")))
        if not match:
            continue
        key = (match["workload"], match["pool"], int(match["threads"]))
        if row.get("error_occurred"):
            errors.append(f"{row['name']}: {row.get('error_message', 'error')}")
            continue
        ms = row["real_time"] * TO_MS[row.get("time_unit", "ns")]
        if row.get("run_type") == "aggregate":
            if row.get("aggregate_name") == stat:
                aggregates[key] = ms
        else:
            samples[key].append(ms)
    if errors:
        sys.exit("benchmarks reported errors:\n  " + "\n  ".join(errors))

    result = {}
    for key in set(samples) | set(aggregates):
        if stat != "min" and key in aggregates:
            result[key] = aggregates[key]
        elif samples.get(key):
            values = samples[key]
            result[key] = {"min": min, "mean": statistics.fmean, "median": statistics.median}[stat](values)
    return result


def format_ms(value: float) -> str:
    if value >= 100:
        return f"{value:.0f}"
    if value >= 10:
        return f"{value:.1f}"
    return f"{value:.2f}"


def context_lines(contexts: list[dict]) -> list[str]:
    lines = []
    for ctx in contexts:
        parts = [
            f"{ctx.get('num_cpus', '?')} CPUs",
            f"compiler {ctx.get('stealpool_compiler', '?')}",
            f"build {ctx.get('stealpool_build_type', '?')}",
            f"load avg {ctx.get('load_avg', '?')}",
        ]
        sizes = [f"{k}={ctx[k]}" for k in ("sort_size", "tree_nodes", "tree_work", "tree_cutoff", "spawn_depth") if k in ctx]
        lines.append("- " + ", ".join(parts) + (f"; workload: {', '.join(sizes)}" if sizes else ""))
    return lines


def report(contexts: list[dict], times: dict[tuple[str, str, int], float], stat: str) -> str:
    out = ["# Stealpool scaling results", "", f"Statistic: {stat} wall-clock time per run.", ""]
    out += context_lines(contexts)
    order = list(WORKLOAD_TITLES)
    workloads = sorted({w for w, _, _ in times}, key=lambda w: (order.index(w) if w in order else len(order), w))
    for workload in workloads:
        pools = [p for p in POOL_ORDER if any(k[0] == workload and k[1] == p for k in times)]
        pools += sorted({p for w, p, _ in times if w == workload} - set(pools))
        threads = sorted({t for w, _, t in times if w == workload})
        out += ["", f"## {WORKLOAD_TITLES.get(workload, workload)}", ""]
        header = ["threads"]
        for pool in pools:
            header += [f"{pool} (ms)", f"{pool} speedup"]
        compare = "work_stealing" in pools and "shared_queue" in pools
        if compare:
            header.append("shared_queue / work_stealing")
        out.append("| " + " | ".join(header) + " |")
        out.append("|" + "|".join("---:" for _ in header) + "|")
        for t in threads:
            cells = [str(t)]
            for pool in pools:
                value = times.get((workload, pool, t))
                base = times.get((workload, pool, 1))
                cells.append(format_ms(value) if value is not None else "-")
                cells.append(f"{base / value:.2f}x" if value and base else "-")
            if compare:
                ws = times.get((workload, "work_stealing", t))
                sq = times.get((workload, "shared_queue", t))
                cells.append(f"{sq / ws:.2f}" if ws and sq else "-")
            out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("inputs", nargs="+", type=Path, help="JSON files from --benchmark_out")
    parser.add_argument("--stat", choices=["median", "mean", "min"], default="median")
    parser.add_argument("-o", "--output", type=Path, help="write markdown here instead of stdout")
    parser.add_argument(
        "--write-json",
        type=Path,
        metavar="PATH",
        help="also write the (single) input JSON to PATH without run date, host name and executable path",
    )
    args = parser.parse_args()
    if args.write_json and len(args.inputs) != 1:
        parser.error("--write-json takes exactly one input file")

    contexts, rows = load(args.inputs)
    times = collect(rows, args.stat)
    if not times:
        sys.exit("no stealpool_bench results found in the input")
    text = report(contexts, times, args.stat)
    if args.write_json:
        write_public_json(args.inputs[0], args.write_json)
    if args.output:
        args.output.write_text(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
