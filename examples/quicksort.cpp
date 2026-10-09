// Parallel quicksort: partition sequentially, sort the two sides as a fork-join pair.
// Partitions are unequal, so the work is unbalanced in a data-dependent way; stealing
// spreads it out.
//
//   example_quicksort [count] [threads]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "stealpool/stealpool.hpp"

namespace {

constexpr std::ptrdiff_t kSequentialCutoff = 2048;

template <class It>
void parallel_quicksort(stealpool::Executor& executor, It first, It last) {
  while (last - first > kSequentialCutoff) {
    // Median of three, then a three-way partition so runs of equal keys cannot cause
    // quadratic recursion.
    auto mid = first + (last - first) / 2;
    const auto pivot = std::max(std::min(*first, *mid), std::min(std::max(*first, *mid), *(last - 1)));
    It lower = std::partition(first, last, [&](const auto& x) { return x < pivot; });
    It upper = std::partition(lower, last, [&](const auto& x) { return !(pivot < x); });

    stealpool::TaskGroup group(executor);
    group.spawn([&executor, first, lower] { parallel_quicksort(executor, first, lower); });
    parallel_quicksort(executor, upper, last);
    group.wait();
    return;
  }
  std::sort(first, last);
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t count = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 10'000'000;
  const std::size_t threads =
      argc > 2 ? std::strtoul(argv[2], nullptr, 10) : stealpool::ThreadPool::default_concurrency();

  std::vector<int> data(count);
  std::mt19937 rng(42);
  for (int& x : data) x = static_cast<int>(rng());
  std::vector<int> expected = data;
  std::sort(expected.begin(), expected.end());

  stealpool::ThreadPool pool(threads);
  pool.submit([&] { parallel_quicksort(pool, data.begin(), data.end()); }).get();

  const auto stats = pool.stats();
  std::printf("sorted %zu ints on %zu threads (tasks %llu, steals %llu)\n", count, threads,
              static_cast<unsigned long long>(stats.tasks_executed),
              static_cast<unsigned long long>(stats.steals));
  if (data != expected) {
    std::fprintf(stderr, "result is not sorted correctly\n");
    return 1;
  }
  return 0;
}
