# Getting started

Every command below is run from the repository root. The README explains what Stealpool does and why;
this page is only how to build, test, use and measure it.

## Requirements

| Tool | Version | Notes |
|---|---|---|
| C++ compiler | C++20 | checked with Apple clang 21, g++ 13.3 and clang 18.1 |
| CMake | 3.25+ for the presets | a plain `cmake -S . -B <dir>` configure needs 3.24+ (it fetches GoogleTest); with tests and benchmarks off, as in the `add_subdirectory` example below, 3.20 works |
| Ninja | any recent | the generator used by every preset |
| Python 3 | any recent | only for benchmark reports and charts (standard library only) |
| Docker | optional | for the Linux image in `docker/Dockerfile` |

The first configure downloads GoogleTest v1.17.0 (tests) and, for Release builds, Google Benchmark v1.9.4
from their pinned release tarballs.

## Build and run the tests

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The result is 314 CTest entries: 299 unit tests, 10 randomized stress tests and 5 example runs. Labels
select parts of the suite:

```sh
ctest --preset debug -LE stress     # everything except the stress suites
ctest --preset debug -L example     # only the example programs
```

## Sanitizers

ThreadSanitizer and AddressSanitizer + UndefinedBehaviorSanitizer each have a preset. The sanitizer flags
are applied to every target, GoogleTest included, so TSan sees every access. The test presets set
`halt_on_error`, so any report fails the test.

```sh
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

## Stress tests and seeds

The `stress` label selects the two randomized suites. With `-V`, each test prints the seed it used and the
operation totals it ran (`[stress] ...` lines):

```sh
ctest --preset tsan -L stress -V
```

`STEALPOOL_STRESS_SEED` (decimal or `0x` hex) replaces the default base seed (set in `tests/stress_util.hpp`), and
`STEALPOOL_STRESS_SCALE` multiplies the amount of work (for example 0.1 for a smoke run, 10 for a soak). A
seed reproduces the sequence of operations, not the thread schedule, so rerun a failing seed a few times
to reproduce a race:

```sh
STEALPOOL_STRESS_SEED=0x1234 STEALPOOL_STRESS_SCALE=0.5 TSAN_OPTIONS=halt_on_error=1 build/tsan/tests/deque_stress_test
```

## Examples

The `release` preset builds the four programs in `examples/`. Each one checks its own result and exits
non-zero on a mismatch.

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release

build/release/examples/example_fib 32 8                          # [n] [threads]
build/release/examples/example_quicksort 10000000 8              # [count] [threads]
build/release/examples/example_tree_sum 2000000 8                # [nodes] [threads]
build/release/examples/example_pipeline_graph 1000000 16 8 3     # [elements] [shards] [threads] [runs]
```

The fib, quicksort and tree programs print how many tasks ran and how many were stolen, for example
`fib(32) = 2178309 on 8 threads (tasks 28657, steals 103)`. Steal counts vary from run to run.

## Using the library

A small program that uses each part of the API:

```cpp
#include <cstdio>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "stealpool/stealpool.hpp"

int main() {
  stealpool::ThreadPool pool(4);  // or ThreadPool() for hardware_concurrency()

  // submit(): returns a move-only TaskHandle<R>; get() waits and rethrows.
  auto answer = pool.submit([] { return 6 * 7; });
  std::printf("submit: %d\n", answer.get());

  // Fork-join with TaskGroup: spawn, do your own share, wait (helps on a worker).
  long left = 0, right = 0;
  {
    stealpool::TaskGroup group(pool);
    group.spawn([&] { left = 1 + 2 + 3; });
    right = 4 + 5;
    group.wait();
  }
  std::printf("task group: %ld\n", left + right);

  // parallel_for: the body is const-callable and runs on several threads at once.
  std::vector<int> squares(1000);
  stealpool::parallel_for(pool, 0, squares.size(), [&](std::size_t i) {
    squares[i] = static_cast<int>(i * i);
  });
  std::printf("parallel_for: %ld\n", std::accumulate(squares.begin(), squares.end(), 0L));

  // TaskGraph: a node runs after all its predecessors; the graph is re-runnable.
  int loaded = 0, a = 0, b = 0, total = 0;
  stealpool::TaskGraph graph;
  auto load = graph.emplace([&] { loaded = 10; });
  auto [left_half, right_half] = graph.emplace([&] { a = loaded + 1; }, [&] { b = loaded + 2; });
  auto merge = graph.emplace([&] { total = a + b; });
  load.precede(left_half, right_half);
  merge.succeed(left_half, right_half);
  graph.run(pool).wait();
  std::printf("graph: %d\n", total);

  // Exceptions: the first one is rethrown by wait()/get(); dependents are skipped.
  auto fail = graph.emplace([] { throw std::runtime_error("boom"); });
  auto after = graph.emplace([] { std::puts("never printed"); });
  fail.precede(after);
  try {
    graph.run(pool).wait();
  } catch (const std::exception& e) {
    std::printf("graph error: %s, skipped %zu\n", e.what(), graph.skipped_in_last_run());
  }
  return 0;
}  // ~ThreadPool drains every scheduled task, then joins the workers
```

It prints:

```text
submit: 42
task group: 15
parallel_for: 332833500
graph: 23
graph error: boom, skipped 1
```

There are no install rules. Add the source tree to your project and link `stealpool::stealpool`. Switch
off Stealpool's tests, examples and benchmarks first; otherwise a Release build of your project would also
fetch Google Benchmark and build the benchmark suite.

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

set(STEALPOOL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STEALPOOL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(STEALPOOL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/stealpool)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE stealpool::stealpool)
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/my_app
```

## Benchmarks

Benchmarks are built only for Release builds. The `bench` label runs every benchmark briefly (one
iteration, two repetitions) on tiny inputs and checks the report script. It verifies results and measures
nothing.

```sh
cmake --preset release
cmake --build --preset release --target stealpool_bench
ctest --preset release -L bench
```

A quick look at one workload, straight from the binary (any Google Benchmark flag works):

```sh
build/release/bench/stealpool_bench --benchmark_filter=task_overhead --threads=1,4,8
```

`scripts/run_benchmarks.sh <label> [flags]` runs the full measurement: 5 repetitions with both pools and
all thread counts randomly interleaved. It writes `<label>.json` and a Markdown table `<label>.md`. Results
go to `bench/results/` by default, and a run with an existing label replaces that label's files. For a
trial run, send the output somewhere under `build/` instead:

```sh
STEALPOOL_BENCH_RESULTS_DIR=build/trial-results scripts/run_benchmarks.sh trial-task-overhead --benchmark_filter=task_overhead
```

Run it only on a quiet machine. The exact commands behind the published results, one process per workload
on macOS and in the Linux container, are listed under "Method and commands" in
[BENCHMARKS.md](BENCHMARKS.md#method-and-commands).

Turn any result file back into the speedup tables (median by default, `--stat mean|min` to change):

```sh
python3 scripts/bench_report.py bench/results/macos-tree-fine.json
```

## Charts

`scripts/plot_scaling.py` redraws the README charts in `docs/assets/` from the first-pass result files.
The output is byte-identical as long as the results are unchanged.

```sh
python3 scripts/plot_scaling.py
python3 scripts/plot_scaling.py --suffix=-run2 --out build/charts-run2     # second pass, elsewhere
```

## Linux in Docker

`docker/Dockerfile` defines an Ubuntu 24.04 image with g++ 13 and 14, clang 18, CMake, Ninja and Python 3.
Build it once:

```sh
docker build -t stealpool-dev docker/
```

Then build and test inside it, with the repository mounted at `/src`. Each configuration gets its own
build directory, so it never collides with the macOS presets.

```sh
# g++, Debug
docker run --rm --mount type=bind,source="$PWD",target=/src -w /src stealpool-dev bash -c \
  'cmake -S . -B build/linux-gcc-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build/linux-gcc-debug && ctest --test-dir build/linux-gcc-debug --output-on-failure'

# g++, ThreadSanitizer
docker run --rm --mount type=bind,source="$PWD",target=/src -w /src stealpool-dev bash -c \
  'cmake -S . -B build/linux-gcc-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTEALPOOL_SANITIZER=thread && cmake --build build/linux-gcc-tsan && TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 ctest --test-dir build/linux-gcc-tsan --output-on-failure'

# the stress suites alone, with their operation totals
docker run --rm --mount type=bind,source="$PWD",target=/src -w /src stealpool-dev bash -c \
  'TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 ctest --test-dir build/linux-gcc-tsan -L stress -V'

# clang, ThreadSanitizer
docker run --rm --mount type=bind,source="$PWD",target=/src -w /src stealpool-dev bash -c \
  'cmake -S . -B build/linux-clang-tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTEALPOOL_SANITIZER=thread && cmake --build build/linux-clang-tsan && TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 ctest --test-dir build/linux-clang-tsan --output-on-failure'
```

## CMake options

| Option | Default | Effect |
|---|---|---|
| `STEALPOOL_BUILD_TESTS` | `ON` | GoogleTest suite and the example and benchmark checks |
| `STEALPOOL_BUILD_EXAMPLES` | `ON` | the programs in `examples/` |
| `STEALPOOL_BUILD_BENCHMARKS` | `ON` for Release, else `OFF` | `stealpool_bench` (Google Benchmark) |
| `STEALPOOL_SANITIZER` | empty | `address` (ASan + UBSan) or `thread` (TSan); anything else is a configure error |
