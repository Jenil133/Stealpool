# Benchmarks

The question these benchmarks answer: does the work-stealing `ThreadPool` keep scaling as
threads are added, while a pool built around one shared, mutex-protected queue flattens out?
Both pools run exactly the same algorithm code; only the scheduler differs.

## What is compared

| Pool | Queueing | Everything else |
|---|---|---|
| `stealpool::ThreadPool` (`work_stealing`) | one lock-free Chase-Lev deque per worker; idle workers steal from a random victim; external submissions go through an injection queue | |
| `stealpool::SharedQueuePool` (`shared_queue`) | one `std::deque` behind one `std::mutex`, shared by all workers and all submitters | same task objects, same `TaskGroup`/`submit`, same helping waits, same idle policy (32 yield-and-retry rounds, then a targeted per-worker park), same drain-on-shutdown |

The baseline is deliberately not a strawman. Its waits help like `ThreadPool`'s (a worker that
waits for a task group keeps running queued tasks), its wakeups are targeted rather than
broadcast, and it takes tasks in the same order as `ThreadPool` would if all per-worker deques
were merged into one: a worker runs its own newest queued task first (like a deque owner) and
otherwise the oldest task in the queue (the biggest piece of a fork-join computation, like a
thief). See `include/stealpool/shared_queue_pool.hpp` and the SharedQueuePool section of
`docs/DESIGN.md` for why simpler orders are not usable with helping waits.

## Workloads (`bench/workloads.hpp`)

All three are fork-join recursions over `TaskGroup`, templated over the pool type and checked
for correctness by `tests/shared_queue_pool_test.cpp` on both pools. Each benchmark also
verifies its own output and reports an error instead of a time if the result is wrong.

* **`parallel_sort`** - merge sort of 10,000,000 random `int`s (fixed seed). Both halves are
  sorted in parallel and the merge itself is parallel (split the longer run at its middle,
  binary-search the other), so the critical path is short and the benchmark measures the
  scheduler, not a sequential top-level step. Sequential cutoffs: `std::sort` below 8192
  elements, `std::merge` below 8192 output elements. Input reset by copy outside the timed
  region (`PauseTiming`).
* **`tree_reduce`** - reduction over an unbalanced random binary tree of 2,097,152 nodes
  (fixed seed). A node of subtree size *s* gives its left child `floor((s-1)*u^2)` nodes
  (*u* uniform in [0, 1)), so sibling subtrees routinely differ by orders of magnitude and the
  tree is far deeper than a balanced one. Every node does 64 rounds of a 64-bit mixing
  function (`node_work`). A task walks down the right spine of its subtree and spawns every
  left subtree of 256 or more nodes as a child task; smaller subtrees are reduced inline.
  Both numbers are options (`--tree_work`, `--tree_cutoff`); see the two reported
  configurations below.
* **`task_overhead`** - a binary fork-join tree of empty tasks of depth 16
  (65,535 spawns per run). Almost pure scheduling cost: spawn, queue, run, join.

Thread counts: 1, 2, 4, 6, 8, 10, 12, 16. Each benchmark constructs its pool outside the
timed loop and runs the root of the computation inside the pool (`pool.submit(...).get()`),
so all waits are helping waits on workers.

Time is wall-clock (`UseRealTime()`): CPU time of the calling thread is meaningless for a pool.
`items_per_second` (elements sorted, tree nodes visited, tasks spawned) is reported as well.

Speedup is computed per pool against the same pool with one thread. The last column of each
table (`shared_queue / work_stealing`) compares the two pools at equal thread count; above 1
means work stealing is faster.

## Commands

Build (Release, which turns `STEALPOOL_BUILD_BENCHMARKS` on; Google Benchmark v1.9.4 is
fetched by CMake from the pinned release tarball):

```sh
cmake --preset release
cmake --build --preset release --target stealpool_bench
```

Full run, five repetitions in random interleaving, `bench/results/<label>.json` and a
markdown table `bench/results/<label>.md` next to it (on a quiet machine only):

```sh
scripts/run_benchmarks.sh macos-default      # or: linux-gcc-default, linux-clang-default, ...
```

which is equivalent to:

```sh
build/release/bench/stealpool_bench \
  --benchmark_repetitions=5 --benchmark_enable_random_interleaving=true \
  --benchmark_out=build/release/bench/results/<label>.json --benchmark_out_format=json
python3 scripts/bench_report.py build/release/bench/results/<label>.json \
  --write-json bench/results/<label>.json -o bench/results/<label>.md
```

The label is the file name, so running a label again replaces its files. Without a label the
script uses `<os>-default` (`darwin-default`, `linux-default`). `STEALPOOL_BENCH_BUILD_DIR`
selects another build directory (configured as Release with benchmarks on) and
`STEALPOOL_BENCH_RESULTS_DIR` another output directory, for example one under `build/` for a
trial run.

`scripts/bench_report.py` uses the median by default (`--stat mean|min` to change). It
accepts several JSON files only if they hold disjoint benchmarks from one run on one machine
(it refuses a benchmark that appears in two files); separate runs get separate reports.
Workload options of `stealpool_bench` (all others go to Google Benchmark, e.g.
`--benchmark_filter=tree_reduce`):

| Option | Default | Meaning |
|---|---|---|
| `--threads=1,2,...` | `1,2,4,6,8,10,12,16` | pool sizes |
| `--sort_size=N` | 10000000 | ints sorted by `parallel_sort` |
| `--tree_nodes=N` | 2097152 | nodes of the `tree_reduce` tree |
| `--tree_work=N` | 64 | `node_work` rounds per tree node |
| `--tree_cutoff=N` | 256 | `tree_reduce` reduces subtrees of fewer than N nodes inline |
| `--spawn_depth=N` | 16 | depth of the `task_overhead` tree (2^N - 1 tasks) |

Google Benchmark's raw JSON stays in the build directory. The copy written to `bench/results/`
keeps the whole `context` except the run date, host name and executable path (dropped by
`bench_report.py --write-json`), so a published result file still records the CPU count,
caches, load average, compiler, build type, Google Benchmark version and every workload
option, without identifying the machine or the day it ran. The markdown table header lists
the same fields.

### Reported configurations

Fixed before any result was looked at; every configuration that is run is reported.

1. **Default suite** - all three workloads with the defaults above
   (`scripts/run_benchmarks.sh <platform>-default`, or one workload per invocation with
   `--benchmark_filter`, which is how the results below were produced).
2. **Fine-grained tree** - `tree_reduce` only, with `--tree_cutoff=32 --tree_work=16`
   (`scripts/run_benchmarks.sh <platform>-tree-fine --benchmark_filter=tree_reduce
   --tree_cutoff=32 --tree_work=16`). Reason: with the defaults every task covers at least
   256 nodes at 64 mixing rounds each, tens of microseconds of work per queue operation, so a
   single mutex-protected queue is not expected to be the bottleneck at these thread counts and
   the default tree mostly measures the hardware ceiling. The fine configuration cuts the work
   per task by about 32x (8x fewer nodes, 4x fewer rounds) so that scheduling cost is a
   visible share of the run, which is where the queue design matters. Same tree, same seed.

Linux runs use the Docker image `stealpool-dev` defined in `docker/Dockerfile` (Ubuntu 24.04
with g++ 13 and 14, clang 18, CMake, Ninja and Python 3). It is not published to a registry,
so build it once from the repository root before the first run:

```sh
docker build -t stealpool-dev docker/
docker run --rm --mount type=bind,source="$PWD",target=/src -w /src stealpool-dev \
  bash -c 'STEALPOOL_BENCH_BUILD_DIR=/src/build/linux-gcc-release CXX=g++ scripts/run_benchmarks.sh linux-gcc-default'
```

Smoke test (part of `ctest` in Release builds, label `bench`): every benchmark runs briefly
(one iteration, two repetitions) on tiny inputs and verifies its result, and the report
script is run on its JSON. It measures nothing.

```sh
ctest --preset release -L bench
```

## Environment to record with every result

CPU model and core layout, OS and kernel, compiler and version, build type and flags, the
exact command, and whether anything else was running. The development machine is an Apple
M5 with 10 cores (4 performance + 6 efficiency) and 16 GB RAM, macOS 26: the heterogeneous
cores are themselves a source of uneven progress, and thread counts above 10 oversubscribe
the machine on purpose.

## Results

Two complete passes per platform, every workload, both pools, threads 1, 2, 4, 6, 8, 10, 12,
16. Each table cell is the median of 5 repetitions in wall-clock milliseconds per run. Every
file that was produced is in `bench/results/` and is reported here:
`{macos,linux-gcc}-{parallel-sort,tree-default,tree-fine,task-overhead}.{json,md}` for the
first pass and the same names ending in `-run2` for the second (32 files).

### Environment

| | macOS | Linux |
|---|---|---|
| Hardware | Apple M5, 10 cores (4 performance + 6 efficiency), 16 GB RAM, fanless | same machine, Docker Desktop VM with 10 vCPUs and about 7.7 GiB RAM; container limited with `--cpus=10` |
| OS | macOS 26.5 | Ubuntu 24.04.5 container (`stealpool-dev`), kernel 7.0.12-linuxkit, aarch64 |
| Compiler | Apple clang 21.0.0 | g++ 13.3.0 |
| Build | `release` preset: Release, `-O3 -DNDEBUG`, CMake 4.4.4, Ninja 1.13.2 | Release, `-O3 -DNDEBUG`, CMake 3.28.3, Ninja 1.11.1 |
| Library | Google Benchmark v1.9.4 | Google Benchmark v1.9.4 |

Machine state: no other benchmark or build was running, but this was a desktop session, not
an idle server. An editor and a web browser stayed open (a few percent CPU each; one browser
process briefly used about one core just before the first family). `pmset -g therm` was
checked before and after every family and never reported a thermal or performance warning
level. The one-minute host load average at the start of a family was 3.0-6.8 for the macOS
families and 4.6-8.1 for the Linux ones. It includes the previous family's threads, which
decay slowly out of the average. The `load_avg` in each JSON context is the value the
benchmark process saw (inside the VM for Linux). The machine has no fan, so sustained
all-core load can lower clocks without any warning being recorded. The second macOS pass,
which started about a minute after the first one ended, was slower than the first at most
points (work-stealing sort: 2-14% slower at every thread count, for example
40.9 vs 46.3 ms at 10 threads). The ratios between the pools moved much less than the times.

### Method and commands

Each workload family runs as its own process (`--benchmark_filter`), with a 45 s pause
between families. Inside a process, `--benchmark_enable_random_interleaving=true` shuffles
every repetition of both pools and all thread counts, so the two pools alternate in time and
any drift in clock speed is spread over both. The second pass ran the four families in
reverse order after a 60 s pause. The order on the machine was macOS pass 1, macOS pass 2,
Linux pass 1, Linux pass 2. Each family took 59-65 s.

macOS (fresh build, then the four families; pass 2 uses labels ending in `-run2` and the
reverse order):

```sh
rm -rf build/release
cmake --preset release && cmake --build --preset release -j 10 && ctest --preset release -L bench
scripts/run_benchmarks.sh macos-parallel-sort --benchmark_filter=parallel_sort
scripts/run_benchmarks.sh macos-tree-default  --benchmark_filter=tree_reduce
scripts/run_benchmarks.sh macos-tree-fine     --benchmark_filter=tree_reduce --tree_cutoff=32 --tree_work=16
scripts/run_benchmarks.sh macos-task-overhead --benchmark_filter=task_overhead
```

Linux (fresh build, then one container per family with the same four filters and labels
`linux-gcc-parallel-sort`, `linux-gcc-tree-default`, `linux-gcc-tree-fine`,
`linux-gcc-task-overhead`, plus `-run2`):

```sh
rm -rf build/linux-gcc-release
docker run --rm --cpus=10 --mount type=bind,source="$PWD",target=/src -w /src stealpool-dev bash -c \
  'cmake -S /src -B /src/build/linux-gcc-release -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release -DSTEALPOOL_BUILD_BENCHMARKS=ON &&
   cmake --build /src/build/linux-gcc-release -j 10 && ctest --test-dir /src/build/linux-gcc-release -L bench'
docker run --rm --cpus=10 --mount type=bind,source="$PWD",target=/src -w /src stealpool-dev bash -c \
  'STEALPOOL_BENCH_BUILD_DIR=/src/build/linux-gcc-release CXX=g++ scripts/run_benchmarks.sh linux-gcc-parallel-sort --benchmark_filter=parallel_sort'
```

Both builds finished with 0 warnings, and `bench.smoke` and `bench.report` passed (2/2).
`run_benchmarks.sh` runs each family with 5 repetitions and random interleaving, and
`bench_report.py` writes the `.json` (context without date, host name and executable path)
and the `.md` tables. The tables below were copied from those `.md` files.

### Summary: shared_queue time / work_stealing time

Above 1 means work stealing is faster at that thread count. Medians of 5 repetitions per
pool and thread count; run 1 and run 2 are the two independent passes.

| Workload | Platform | Run | 1 | 2 | 4 | 6 | 8 | 10 | 12 | 16 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Parallel sort | macOS | 1 | 1.00 | 1.00 | 1.00 | 1.04 | 1.07 | 1.10 | 1.06 | 1.07 |
| Parallel sort | macOS | 2 | 0.98 | 1.08 | 1.02 | 1.04 | 1.00 | 1.09 | 1.14 | 1.09 |
| Parallel sort | Linux | 1 | 1.04 | 0.99 | 1.04 | 1.03 | 1.03 | 0.99 | 1.09 | 0.98 |
| Parallel sort | Linux | 2 | 1.01 | 1.03 | 1.02 | 0.97 | 0.97 | 1.13 | 1.10 | 0.96 |
| Tree, default | macOS | 1 | 1.09 | 1.06 | 0.84 | 1.20 | 1.01 | 1.03 | 1.04 | 1.02 |
| Tree, default | macOS | 2 | 1.01 | 0.99 | 1.09 | 1.02 | 1.05 | 1.04 | 1.06 | 1.04 |
| Tree, default | Linux | 1 | 0.94 | 1.00 | 1.10 | 1.05 | 0.99 | 1.04 | 0.98 | 1.06 |
| Tree, default | Linux | 2 | 1.00 | 1.01 | 0.92 | 0.94 | 1.02 | 0.99 | 1.03 | 1.04 |
| Tree, fine-grained | macOS | 1 | 1.02 | 2.00 | 4.08 | 5.54 | 7.17 | 6.81 | 6.49 | 6.58 |
| Tree, fine-grained | macOS | 2 | 1.03 | 2.04 | 3.57 | 5.83 | 6.44 | 6.52 | 6.73 | 6.31 |
| Tree, fine-grained | Linux | 1 | 1.00 | 2.00 | 4.85 | 5.93 | 7.87 | 9.96 | 9.39 | 10.31 |
| Tree, fine-grained | Linux | 2 | 1.04 | 1.83 | 3.83 | 6.25 | 7.85 | 9.72 | 9.05 | 10.83 |
| Task overhead | macOS | 1 | 1.35 | 5.41 | 12.76 | 17.12 | 17.66 | 17.21 | 16.25 | 15.01 |
| Task overhead | macOS | 2 | 1.30 | 5.35 | 12.11 | 21.10 | 20.51 | 21.54 | 17.47 | 17.58 |
| Task overhead | Linux | 1 | 1.64 | 30.97 | 12.16 | 21.28 | 24.39 | 34.76 | 47.26 | 84.22 |
| Task overhead | Linux | 2 | 1.67 | 32.76 | 12.59 | 21.49 | 29.39 | 38.67 | 52.11 | 78.26 |

### Full tables (run 1)

Time in ms, speedup against the same pool with one thread, and the ratio. The run 2 tables
have the same layout in the `-run2.md` files.

#### Parallel merge sort (10,000,000 ints)

macOS (`bench/results/macos-parallel-sort.md`):

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 316 | 1.00x | 316 | 1.00x | 1.00 |
| 2 | 160 | 1.97x | 160 | 1.97x | 1.00 |
| 4 | 81.1 | 3.90x | 81.3 | 3.89x | 1.00 |
| 6 | 59.8 | 5.28x | 62.3 | 5.07x | 1.04 |
| 8 | 48.2 | 6.55x | 51.7 | 6.11x | 1.07 |
| 10 | 40.9 | 7.72x | 44.8 | 7.05x | 1.10 |
| 12 | 42.5 | 7.43x | 44.9 | 7.03x | 1.06 |
| 16 | 43.0 | 7.34x | 46.0 | 6.87x | 1.07 |

Linux (`bench/results/linux-gcc-parallel-sort.md`):

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 561 | 1.00x | 583 | 1.00x | 1.04 |
| 2 | 286 | 1.96x | 285 | 2.05x | 0.99 |
| 4 | 157 | 3.58x | 163 | 3.57x | 1.04 |
| 6 | 110 | 5.11x | 113 | 5.18x | 1.03 |
| 8 | 87.5 | 6.41x | 90.1 | 6.48x | 1.03 |
| 10 | 80.9 | 6.94x | 80.1 | 7.29x | 0.99 |
| 12 | 74.1 | 7.58x | 81.0 | 7.20x | 1.09 |
| 16 | 77.0 | 7.29x | 75.4 | 7.74x | 0.98 |

#### Unbalanced tree reduction, default (2,097,152 nodes, cutoff 256, 64 rounds per node)

macOS (`bench/results/macos-tree-default.md`):

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 121 | 1.00x | 132 | 1.00x | 1.09 |
| 2 | 64.7 | 1.88x | 68.4 | 1.93x | 1.06 |
| 4 | 39.0 | 3.11x | 32.9 | 4.02x | 0.84 |
| 6 | 27.6 | 4.39x | 33.2 | 3.99x | 1.20 |
| 8 | 23.0 | 5.27x | 23.2 | 5.69x | 1.01 |
| 10 | 20.4 | 5.94x | 21.0 | 6.30x | 1.03 |
| 12 | 20.9 | 5.81x | 21.8 | 6.08x | 1.04 |
| 16 | 20.3 | 5.99x | 20.7 | 6.40x | 1.02 |

Linux (`bench/results/linux-gcc-tree-default.md`):

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 132 | 1.00x | 124 | 1.00x | 0.94 |
| 2 | 62.5 | 2.12x | 62.5 | 1.99x | 1.00 |
| 4 | 33.9 | 3.90x | 37.4 | 3.32x | 1.10 |
| 6 | 27.8 | 4.76x | 29.2 | 4.25x | 1.05 |
| 8 | 25.6 | 5.16x | 25.3 | 4.91x | 0.99 |
| 10 | 23.1 | 5.73x | 23.9 | 5.19x | 1.04 |
| 12 | 22.4 | 5.91x | 22.0 | 5.65x | 0.98 |
| 16 | 22.4 | 5.89x | 23.7 | 5.25x | 1.06 |

#### Unbalanced tree reduction, fine-grained (cutoff 32, 16 rounds per node)

macOS (`bench/results/macos-tree-fine.md`):

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 28.4 | 1.00x | 28.8 | 1.00x | 1.02 |
| 2 | 14.5 | 1.96x | 28.9 | 1.00x | 2.00 |
| 4 | 7.89 | 3.60x | 32.2 | 0.90x | 4.08 |
| 6 | 8.01 | 3.54x | 44.4 | 0.65x | 5.54 |
| 8 | 7.18 | 3.95x | 51.5 | 0.56x | 7.17 |
| 10 | 7.31 | 3.88x | 49.8 | 0.58x | 6.81 |
| 12 | 7.26 | 3.91x | 47.1 | 0.61x | 6.49 |
| 16 | 7.07 | 4.01x | 46.6 | 0.62x | 6.58 |

Linux (`bench/results/linux-gcc-tree-fine.md`):

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 24.6 | 1.00x | 24.6 | 1.00x | 1.00 |
| 2 | 12.3 | 2.00x | 24.6 | 1.00x | 2.00 |
| 4 | 6.35 | 3.88x | 30.8 | 0.80x | 4.85 |
| 6 | 6.09 | 4.04x | 36.1 | 0.68x | 5.93 |
| 8 | 5.24 | 4.69x | 41.3 | 0.60x | 7.87 |
| 10 | 4.64 | 5.30x | 46.2 | 0.53x | 9.96 |
| 12 | 4.97 | 4.95x | 46.6 | 0.53x | 9.39 |
| 16 | 5.36 | 4.59x | 55.3 | 0.45x | 10.31 |

#### Task overhead (65,535 empty tasks)

macOS (`bench/results/macos-task-overhead.md`):

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.87 | 1.00x | 2.52 | 1.00x | 1.35 |
| 2 | 1.06 | 1.77x | 5.71 | 0.44x | 5.41 |
| 4 | 0.76 | 2.45x | 9.73 | 0.26x | 12.76 |
| 6 | 0.78 | 2.39x | 13.4 | 0.19x | 17.12 |
| 8 | 0.77 | 2.41x | 13.7 | 0.18x | 17.66 |
| 10 | 0.78 | 2.41x | 13.4 | 0.19x | 17.21 |
| 12 | 0.80 | 2.34x | 13.0 | 0.19x | 16.25 |
| 16 | 0.88 | 2.13x | 13.2 | 0.19x | 15.01 |

Linux (`bench/results/linux-gcc-task-overhead.md`):

| threads | work_stealing (ms) | work_stealing speedup | shared_queue (ms) | shared_queue speedup | shared_queue / work_stealing |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.16 | 1.00x | 1.90 | 1.00x | 1.64 |
| 2 | 0.66 | 1.75x | 20.5 | 0.09x | 30.97 |
| 4 | 0.48 | 2.41x | 5.84 | 0.33x | 12.16 |
| 6 | 0.35 | 3.32x | 7.43 | 0.26x | 21.28 |
| 8 | 0.38 | 3.08x | 9.16 | 0.21x | 24.39 |
| 10 | 0.34 | 3.40x | 11.8 | 0.16x | 34.76 |
| 12 | 0.34 | 3.44x | 15.9 | 0.12x | 47.26 |
| 16 | 0.33 | 3.51x | 27.8 | 0.07x | 84.22 |

### Interpretation

**Does work stealing scale better than the shared queue past 8 threads?** It depends on how
much work each task carries. Both runs on both platforms agree on this.

| Workload | macOS, 8-16 threads | Linux, 8-16 threads | Supports "better scaling past 8 threads"? |
|---|---|---|---|
| Tree, fine-grained | ratio 6.31-7.17; work stealing 3.59-4.09x speedup, shared queue 0.56-0.62x | ratio 7.85-10.83; 4.59-5.30x vs 0.45-0.67x | **Yes**, on both platforms and in both runs |
| Task overhead | ratio 15.0-21.5 | ratio 24.4-84.2 | **Yes** (pure scheduling cost, not an application workload) |
| Parallel sort | ratio 1.00-1.14; 6.69-7.72x vs 5.77-7.05x at 10-16 threads | ratio 0.96-1.13, no consistent winner | **Weakly on macOS, no on Linux** |
| Tree, default | ratio 1.01-1.06 | ratio 0.98-1.06 | **No**: both pools reach the same ceiling |

The ratios are medians. As a stricter check, compare the slowest work-stealing repetition
with the fastest shared-queue repetition at 8-16 threads. Work stealing still wins on the
fine-grained tree by at least 4.9x on macOS and 6.5x on Linux, and on task overhead by at
least 11.9x and 19.6x. On sort and the default tree, the repetition ranges of the two pools
overlap at almost every one of these thread counts. The two exceptions are macOS sort run 1
at 8 and 10 threads, where even the slowest work-stealing repetition beat the fastest
shared-queue one (by 3% and 6%).

What the data shows:

* **Single-queue lock contention is the dominant effect.** The two pools run the same task
  objects, the same helping waits, the same idle policy and the same pop order. The only
  difference is that every `schedule()` and every pop of `SharedQueuePool` takes one mutex.
  On the fine-grained tree, the shared queue gets no faster with a second worker in run 1
  (macOS 28.8 vs 28.9 ms, Linux 24.6 vs 24.6 ms) and then gets slower: on macOS up to 51.5 ms
  at 8 threads, where it levels off; on Linux all the way to 55.3 ms at 16 threads. That is
  1.8-2.2x its one-thread time. Added workers mostly add lock traffic. On task overhead the
  shared queue is already 1.3-1.7x slower with one thread, where its lock is never
  contended: locking a mutex for every push and pop costs more than a Chase-Lev push and
  pop. With 2 or more threads it is 5-84x slower. On Linux the shared queue's task-overhead
  time keeps growing with threads (9.16 ms at 8, 27.8 ms at 16 in run 1). On macOS it stays
  flat at 12.6-13.7 ms from 6 threads up.
* **Coarse tasks hide the queue.** With the defaults, a tree task covers at least 256 nodes
  at 64 mixing rounds each. A sort leaf sorts or merges up to 8192 elements. Queue operations
  are then rare compared with the work, so one mutex is not a bottleneck at 10 cores, and both
  pools reach the same ceiling: a peak speedup of 5.5-6.4x on the default tree and 6.4-7.7x
  on sort, depending on pool, platform and run. An uneven workload alone does not separate
  the pools. A shared queue also balances load perfectly, because any idle worker takes any
  queued task. Work stealing pays off when tasks are small enough that the cost per queue
  operation matters.
* **LIFO locality is not what separates them here.** By design, the baseline also runs a
  worker's own newest task first (see `docs/DESIGN.md`), so both pools get the depth-first,
  cache-friendly order of a deque owner. These benchmarks do not isolate locality. The
  difference that remains is the per-operation cost and cache-line traffic of one shared lock
  and one shared container. A work-stealing owner pushes and pops at the bottom of its
  own deque without a lock, and thieves touch only the top.
* **Heterogeneous P/E cores flatten the scaling of both pools.** On macOS, sort with work
  stealing reaches 3.90x at 4 threads (the 4 performance cores), then adds less per thread:
  5.28x at 6, 6.55x at 8, 7.72x at 10, not 10x. This fits efficiency cores being slower;
  per-core placement was not measured. The same effect makes single repetitions noisy. The
  one-thread default tree took 118-150 ms depending on the repetition, and macOS run 1 has
  bimodal repetitions at 4 and 6 threads (for example work stealing at 4 threads: 32.4-59.9
  ms). That produces the 0.84 and 1.20 ratios there, which run 2 does not repeat (1.09,
  1.02). On the fine-grained tree, work stealing on macOS stays at 3.5-4.1x from 6 threads
  on, while in the Linux VM it reaches 4.6-5.3x at 8-16 threads. The cause of that difference
  was not investigated.
* **Oversubscription (12 and 16 threads on 10 cores) does not hurt work stealing much.** Its
  sort and tree times at 12 and 16 threads are within 0.90-1.16x of its 10-thread time. Its
  sub-millisecond task-overhead runs stay within 0.87-1.25x. Idle workers yield and then
  park instead of spinning. The shared queue changes little on macOS (0.93-1.09x). On Linux its
  task-overhead time at 16 threads is 2.3-2.5x its 10-thread time. More threads compete for
  the one mutex, and with more threads than vCPUs a lock holder can be descheduled; that last
  point is plausible but was not measured.
* **Unexplained point:** on Linux, the shared queue with exactly 2 threads takes about 20 ms
  for the task-overhead tree in both runs (20.5 and 21.4 ms). That is slower than with 4
  threads (5.84 ms). It is reported as measured. One possible cause, the two workers handing
  the mutex back and forth inside the VM, was not investigated.

**Verdict on the claim** "better scaling past 8 threads on parallel sort and tree
traversal": the tree traversal part holds for fine-grained tasks on both macOS and Linux.
There, work stealing is 6.3-10.8x faster at 8-16 threads, and the shared queue runs slower
than it does on one thread. It does not hold for the default, coarse-grained tree, where
both pools are within 6% of each other. The parallel sort part does not hold as a general
statement. Work stealing was 0-14% faster on macOS at 8-16 threads, with overlapping
repetition ranges, and Linux showed no consistent difference. A summary these numbers fully
support:

> Benchmarked against a single shared-queue pool with Google Benchmark on one 10-core
> Apple M5 (macOS and a Linux VM): at 8-16 threads, work stealing ran fine-grained tree
> traversal over 6x faster and cut task-scheduling overhead over 15x, while coarse-grained
> parallel sort stayed within 14% of the baseline.

### Caveats

* One machine, two operating systems on it: the Linux numbers come from a VM on the same
  Apple M5, not from separate x86 hardware. vCPUs map onto the same mix of performance and
  efficiency cores.
* Desktop session, not an idle server (see Environment). The machine is fanless, and the
  second pass on each platform ran on an already warm machine. Five repetitions per point;
  repetitions of sort and the default tree spread by 20% or more at many points, with single
  outliers up to 2.7x the median (one Linux sort repetition at 2 threads), so median
  differences under about 10% there are not meaningful.
* Thread counts above 10 oversubscribe the machine on purpose, so they measure behaviour
  under oversubscription, not more parallel hardware.
* The baseline is a single mutex-protected `std::deque` with the same pop order and idle
  policy as `ThreadPool`. A lock-free MPMC queue or a different baseline order would give
  different numbers.
* Per-core utilisation and lock hold times were not measured; the mechanisms above are
  inferred from how time changes with thread count.
