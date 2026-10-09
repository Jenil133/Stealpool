// A multi-step job expressed as a task graph: generate -> split into shards ->
// per shard (sort, then histogram) -> merge the sorted shards and the histograms ->
// verify. Each shard's two steps form their own chain, all shards run in parallel, and
// the merge steps wait for every shard. The graph is built once and run several times
// with different seeds.
//
//   example_pipeline_graph [elements] [shards] [threads] [runs]
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "stealpool/stealpool.hpp"

namespace {

constexpr std::size_t kBuckets = 64;

struct Job {
  std::uint64_t seed = 0;
  std::vector<std::uint32_t> input;
  std::vector<std::vector<std::uint32_t>> shards;
  std::vector<std::vector<std::uint64_t>> shard_histograms;
  std::vector<std::uint32_t> sorted;
  std::vector<std::uint64_t> histogram;
};

std::size_t bucket_of(std::uint32_t value) { return value % kBuckets; }

}  // namespace

int main(int argc, char** argv) {
  const std::size_t elements = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
  const std::size_t shards = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 16;
  const std::size_t threads =
      argc > 3 ? std::strtoull(argv[3], nullptr, 10) : stealpool::ThreadPool::default_concurrency();
  const int runs = argc > 4 ? std::atoi(argv[4]) : 3;
  if (shards == 0 || threads == 0) {
    std::fprintf(stderr, "shards and threads must be positive\n");
    return 2;
  }

  stealpool::ThreadPool pool(threads);
  Job job;
  job.shards.resize(shards);
  job.shard_histograms.resize(shards);

  stealpool::TaskGraph graph;
  auto generate = graph.emplace([&] {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(job.seed));
    job.input.resize(elements);
    for (auto& value : job.input) value = static_cast<std::uint32_t>(rng());
  });
  auto merge_sorted = graph.emplace([&] {
    // Repeated pairwise merging of the sorted shards.
    job.sorted.clear();
    for (const auto& shard : job.shards) {
      std::vector<std::uint32_t> merged(job.sorted.size() + shard.size());
      std::merge(job.sorted.begin(), job.sorted.end(), shard.begin(), shard.end(), merged.begin());
      job.sorted.swap(merged);
    }
  });
  auto merge_histograms = graph.emplace([&] {
    job.histogram.assign(kBuckets, 0);
    for (const auto& partial : job.shard_histograms) {
      for (std::size_t b = 0; b < kBuckets; ++b) job.histogram[b] += partial[b];
    }
  });

  for (std::size_t s = 0; s < shards; ++s) {
    auto sort_shard = graph.emplace([&, s] {
      const std::size_t begin = elements * s / shards;
      const std::size_t end = elements * (s + 1) / shards;
      auto& shard = job.shards[s];
      shard.assign(job.input.begin() + static_cast<std::ptrdiff_t>(begin),
                   job.input.begin() + static_cast<std::ptrdiff_t>(end));
      std::sort(shard.begin(), shard.end());
    });
    auto count_shard = graph.emplace([&, s] {
      auto& counts = job.shard_histograms[s];
      counts.assign(kBuckets, 0);
      for (std::uint32_t value : job.shards[s]) ++counts[bucket_of(value)];
    });
    generate.precede(sort_shard);
    sort_shard.precede(count_shard, merge_sorted);
    count_shard.precede(merge_histograms);
  }

  for (int run = 0; run < runs; ++run) {
    job.seed = 1000 + static_cast<std::uint64_t>(run);
    graph.run(pool).wait();

    // Check against a sequential computation.
    std::vector<std::uint32_t> expected = job.input;
    std::sort(expected.begin(), expected.end());
    std::vector<std::uint64_t> expected_histogram(kBuckets, 0);
    for (std::uint32_t value : expected) ++expected_histogram[bucket_of(value)];
    if (job.sorted != expected || job.histogram != expected_histogram) {
      std::fprintf(stderr, "run %d: pipeline result mismatch\n", run);
      return 1;
    }
  }
  std::printf("pipeline graph: %zu nodes, %d runs of %zu elements in %zu shards on %zu threads: ok\n",
              graph.size(), runs, elements, shards, threads);
  return 0;
}
