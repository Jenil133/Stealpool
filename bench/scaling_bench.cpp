// Scaling study: the work-stealing ThreadPool against the SharedQueuePool baseline on the
// same fork-join workloads (bench/workloads.hpp), for a range of thread counts.
//
//   stealpool_bench [workload options] [Google Benchmark options]
//
// Workload options (consumed here, everything else goes to Google Benchmark):
//   --threads=1,2,4      thread counts to run (default 1,2,4,6,8,10,12,16)
//   --sort_size=N        ints to sort (default 10000000)
//   --tree_nodes=N       nodes in the unbalanced tree (default 2097152)
//   --tree_work=N        node_work() rounds per tree node (default 64)
//   --tree_cutoff=N      subtrees smaller than N nodes are reduced inline (default 256)
//   --spawn_depth=N      depth of the empty-task fork-join tree (default 16)
//
// Benchmark names are "<workload>/<pool>/threads:<n>/real_time" with pool
// "work_stealing" or "shared_queue". scripts/bench_report.py turns the JSON output into
// speedup tables; docs/BENCHMARKS.md has the exact commands.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "stealpool/shared_queue_pool.hpp"
#include "stealpool/thread_pool.hpp"
#include "workloads.hpp"

namespace {

namespace sb = stealpool::bench;

struct Options {
  std::vector<std::int64_t> threads{1, 2, 4, 6, 8, 10, 12, 16};
  std::size_t sort_size = 10'000'000;
  std::size_t tree_nodes = std::size_t{1} << 21;
  unsigned tree_work = 64;
  std::uint32_t tree_cutoff = sb::TreeParams{}.sequential_cutoff;
  unsigned spawn_depth = 16;
};

Options g_options;

template <class Pool>
constexpr const char* pool_name();
template <>
constexpr const char* pool_name<stealpool::ThreadPool>() {
  return "work_stealing";
}
template <>
constexpr const char* pool_name<stealpool::SharedQueuePool>() {
  return "shared_queue";
}

// Inputs are generated once per process and shared by every benchmark, so all pools and
// thread counts sort the same numbers and walk the same tree.
const std::vector<int>& sort_input() {
  static const std::vector<int> input = sb::random_ints(g_options.sort_size, 42);
  return input;
}

const sb::Tree& tree_input() {
  static const sb::Tree tree = sb::build_unbalanced_tree(g_options.tree_nodes, 7);
  return tree;
}

std::uint64_t tree_expected() {
  static const std::uint64_t expected = sb::reduce_tree_sequential(tree_input(), 0, g_options.tree_work);
  return expected;
}

template <class Pool>
void BM_ParallelSort(benchmark::State& state) {
  Pool pool(static_cast<std::size_t>(state.range(0)));
  const std::vector<int>& input = sort_input();
  std::vector<int> values;
  std::vector<int> scratch(input.size());
  for (auto _ : state) {
    state.PauseTiming();
    values = input;
    state.ResumeTiming();
    pool.submit([&] { sb::parallel_sort(pool, values, scratch); }).get();
  }
  if (!std::is_sorted(values.begin(), values.end())) state.SkipWithError("output is not sorted");
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(input.size()));
}

template <class Pool>
void BM_TreeReduce(benchmark::State& state) {
  Pool pool(static_cast<std::size_t>(state.range(0)));
  const sb::Tree& tree = tree_input();
  const sb::TreeParams params{.work_rounds = g_options.tree_work, .sequential_cutoff = g_options.tree_cutoff};
  std::uint64_t total = 0;
  for (auto _ : state) {
    total = pool.submit([&] { return sb::reduce_tree(pool, tree, 0, params); }).get();
    benchmark::DoNotOptimize(total);
  }
  if (total != tree_expected()) state.SkipWithError("tree reduction returned a wrong sum");
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(tree.nodes.size()));
}

template <class Pool>
void BM_TaskOverhead(benchmark::State& state) {
  Pool pool(static_cast<std::size_t>(state.range(0)));
  const unsigned depth = g_options.spawn_depth;
  const std::uint64_t expected = (std::uint64_t{1} << depth) - 1;
  std::uint64_t tasks = 0;
  for (auto _ : state) {
    tasks = pool.submit([&] { return sb::spawn_tree(pool, depth); }).get();
  }
  if (tasks != expected) state.SkipWithError("spawn tree lost tasks");
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(expected));
}

template <class Pool>
void register_pool_benchmarks() {
  const std::string pool = pool_name<Pool>();
  auto configure = [](benchmark::internal::Benchmark* b) {
    b->ArgName("threads")->UseRealTime()->Unit(benchmark::kMillisecond);
    for (std::int64_t t : g_options.threads) b->Arg(t);
  };
  configure(benchmark::RegisterBenchmark("parallel_sort/" + pool, BM_ParallelSort<Pool>));
  configure(benchmark::RegisterBenchmark("tree_reduce/" + pool, BM_TreeReduce<Pool>));
  configure(benchmark::RegisterBenchmark("task_overhead/" + pool, BM_TaskOverhead<Pool>));
}

bool parse_unsigned(std::string_view text, std::size_t& out) {
  if (text.empty()) return false;
  std::size_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<std::size_t>(c - '0');
  }
  out = value;
  return true;
}

bool parse_thread_list(std::string_view text, std::vector<std::int64_t>& out) {
  out.clear();
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    std::size_t value = 0;
    if (!parse_unsigned(text.substr(0, comma), value) || value == 0) return false;
    out.push_back(static_cast<std::int64_t>(value));
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
  }
  return !out.empty();
}

// Removes the workload options from argv. Returns false on a malformed value.
bool consume_options(int& argc, char** argv) {
  int kept = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto value_of = [&](std::string_view flag, std::string_view& value) {
      if (arg.substr(0, flag.size()) != flag) return false;
      value = arg.substr(flag.size());
      return true;
    };
    std::string_view value;
    std::size_t number = 0;
    bool ok = true;
    if (value_of("--threads=", value)) {
      ok = parse_thread_list(value, g_options.threads);
    } else if (value_of("--sort_size=", value)) {
      ok = parse_unsigned(value, g_options.sort_size);
    } else if (value_of("--tree_nodes=", value)) {
      ok = parse_unsigned(value, g_options.tree_nodes) && g_options.tree_nodes > 0 &&
           g_options.tree_nodes <= 0x7fffffff;
    } else if (value_of("--tree_work=", value)) {
      ok = parse_unsigned(value, number);
      g_options.tree_work = static_cast<unsigned>(number);
    } else if (value_of("--tree_cutoff=", value)) {
      ok = parse_unsigned(value, number) && number >= 1 && number <= 0x7fffffff;
      g_options.tree_cutoff = static_cast<std::uint32_t>(number);
    } else if (value_of("--spawn_depth=", value)) {
      ok = parse_unsigned(value, number) && number >= 1 && number <= 30;
      g_options.spawn_depth = static_cast<unsigned>(number);
    } else {
      argv[kept++] = argv[i];
      continue;
    }
    if (!ok) {
      std::fprintf(stderr, "invalid value in '%s'\n", argv[i]);
      return false;
    }
  }
  argc = kept;
  argv[argc] = nullptr;
  return true;
}

std::string thread_list() {
  std::string out;
  for (std::int64_t t : g_options.threads) {
    if (!out.empty()) out += ',';
    out += std::to_string(t);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (!consume_options(argc, argv)) return 2;

  // Recorded in the JSON "context" so every result file says what it measured.
  benchmark::AddCustomContext("stealpool_build_type", STEALPOOL_BENCH_BUILD_TYPE);
  benchmark::AddCustomContext("stealpool_compiler", STEALPOOL_BENCH_COMPILER);
  benchmark::AddCustomContext("hardware_concurrency", std::to_string(std::thread::hardware_concurrency()));
  benchmark::AddCustomContext("threads", thread_list());
  benchmark::AddCustomContext("sort_size", std::to_string(g_options.sort_size));
  benchmark::AddCustomContext("tree_nodes", std::to_string(g_options.tree_nodes));
  benchmark::AddCustomContext("tree_work", std::to_string(g_options.tree_work));
  benchmark::AddCustomContext("tree_cutoff", std::to_string(g_options.tree_cutoff));
  benchmark::AddCustomContext("spawn_depth", std::to_string(g_options.spawn_depth));

  register_pool_benchmarks<stealpool::ThreadPool>();
  register_pool_benchmarks<stealpool::SharedQueuePool>();

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
