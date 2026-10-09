#!/usr/bin/env bash
# Builds the release preset and runs the full scaling suite, writing <label>.json to
# bench/results/ and a markdown table (<label>.md) next to it.
#
#   scripts/run_benchmarks.sh [label] [extra stealpool_bench / Google Benchmark flags...]
#
# Run it on a quiet machine: the numbers are only meaningful when nothing else is
# competing for the cores. The label (default: <os>-default, e.g. darwin-default) is the
# file name, so a rerun with the same label replaces the previous result.
#
# Google Benchmark's raw output stays in the build directory; the published JSON and
# markdown leave out the run date, host name and executable path (scripts/bench_report.py).
#
# Environment:
#   STEALPOOL_BENCH_BUILD_DIR    build directory (default: build/release via the release preset)
#   STEALPOOL_BENCH_RESULTS_DIR  output directory (default: bench/results)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
label="${1:-$(uname -s | tr '[:upper:]' '[:lower:]')-default}"
shift || true
build_dir="${STEALPOOL_BENCH_BUILD_DIR:-$root/build/release}"
results_dir="${STEALPOOL_BENCH_RESULTS_DIR:-$root/bench/results}"
raw="$build_dir/bench/results/${label}.json"
out="$results_dir/${label}.json"

if [[ "$build_dir" == "$root/build/release" ]]; then
  (cd "$root" && cmake --preset release)
else
  cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTEALPOOL_BUILD_BENCHMARKS=ON
fi
cmake --build "$build_dir" --target stealpool_bench

mkdir -p "$results_dir" "$(dirname "$raw")"
"$build_dir/bench/stealpool_bench" \
  --benchmark_repetitions=5 \
  --benchmark_enable_random_interleaving=true \
  --benchmark_out="$raw" \
  --benchmark_out_format=json \
  "$@"

python3 "$root/scripts/bench_report.py" "$raw" --write-json "$out" -o "${out%.json}.md"
echo "results: $out"
echo "table:   ${out%.json}.md"
