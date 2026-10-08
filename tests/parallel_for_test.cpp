#include "stealpool/parallel_for.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "stealpool/thread_pool.hpp"

namespace {

using stealpool::ThreadPool;

// (workers, count, grain)
class ParallelForCoverageTest
    : public ::testing::TestWithParam<std::tuple<std::size_t, std::size_t, std::size_t>> {};

TEST_P(ParallelForCoverageTest, VisitsEveryIndexExactlyOnce) {
  const auto [workers, count, grain] = GetParam();
  ThreadPool pool(workers);
  std::vector<std::atomic<int>> visits(count);
  stealpool::parallel_for(pool, 0, count, grain, [&](std::size_t i) { visits[i].fetch_add(1); });
  for (std::size_t i = 0; i < count; ++i) ASSERT_EQ(visits[i].load(), 1) << "index " << i;
}

TEST_P(ParallelForCoverageTest, BlockedRangesAreDisjointAndBounded) {
  const auto [workers, count, grain] = GetParam();
  ThreadPool pool(workers);
  std::mutex mutex;
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  const std::size_t first = 5;  // non-zero origin
  stealpool::parallel_for_blocked(pool, first, first + count, grain, [&](std::size_t lo, std::size_t hi) {
    std::lock_guard lock(mutex);
    ranges.emplace_back(lo, hi);
  });
  std::sort(ranges.begin(), ranges.end());
  std::size_t expected_lo = first;
  for (const auto& [lo, hi] : ranges) {
    ASSERT_EQ(lo, expected_lo);
    ASSERT_LT(lo, hi);
    if (grain != 0) {
      ASSERT_LE(hi - lo, grain);
    }
    expected_lo = hi;
  }
  EXPECT_EQ(expected_lo, first + count);
  if (count == 0) {
    EXPECT_TRUE(ranges.empty());
  }
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, ParallelForCoverageTest,
    ::testing::Combine(::testing::Values(1, 2, 4), ::testing::Values(0, 1, 7, 1000, 100003),
                       ::testing::Values(0, 1, 3, 4096)),
    [](const auto& info) {
      return std::to_string(std::get<0>(info.param)) + "Workers_" + std::to_string(std::get<1>(info.param)) +
             "Items_Grain" + std::to_string(std::get<2>(info.param));
    });

TEST(ParallelForTest, DefaultGrainOverload) {
  ThreadPool pool(3);
  std::vector<std::uint64_t> out(10000);
  stealpool::parallel_for(pool, 0, out.size(), [&](std::size_t i) { out[i] = i * i; });
  for (std::size_t i = 0; i < out.size(); ++i) ASSERT_EQ(out[i], i * i);
}

TEST(ParallelForTest, EmptyAndReversedRangesDoNothing) {
  ThreadPool pool(2);
  std::atomic<int> calls{0};
  stealpool::parallel_for(pool, 10, 10, [&](std::size_t) { calls.fetch_add(1); });
  stealpool::parallel_for(pool, 10, 3, [&](std::size_t) { calls.fetch_add(1); });
  EXPECT_EQ(calls.load(), 0);
}

TEST(ParallelForTest, PropagatesException) {
  ThreadPool pool(4);
  std::atomic<int> visited{0};
  EXPECT_THROW(stealpool::parallel_for(pool, 0, 1000, 1,
                                       [&](std::size_t i) {
                                         visited.fetch_add(1);
                                         if (i == 777) throw std::runtime_error("index 777");
                                       }),
               std::runtime_error);
  // Policy: chunks are not cancelled, so every index other than the throwing one ran.
  EXPECT_EQ(visited.load(), 1000);
}

TEST(ParallelForTest, NestedLoopsInsideWorkers) {
  ThreadPool pool(4);
  constexpr std::size_t kRows = 64;
  constexpr std::size_t kCols = 500;
  std::vector<std::atomic<int>> cells(kRows * kCols);
  pool.submit([&] {
        stealpool::parallel_for(pool, 0, kRows, 1, [&](std::size_t r) {
          stealpool::parallel_for(pool, 0, kCols, 16, [&](std::size_t c) { cells[r * kCols + c].fetch_add(1); });
        });
      })
      .get();
  for (std::size_t i = 0; i < cells.size(); ++i) ASSERT_EQ(cells[i].load(), 1) << "cell " << i;
}

TEST(ParallelForTest, UnevenIterationCosts) {
  ThreadPool pool(4);
  // Iteration i costs ~i^2/1000 steps: the last chunks are far heavier than the first.
  constexpr std::size_t kCount = 2000;
  std::vector<std::uint64_t> result(kCount);
  stealpool::parallel_for(pool, 0, kCount, 8, [&](std::size_t i) {
    std::uint64_t acc = i;
    const std::size_t steps = i * i / 1000;
    for (std::size_t s = 0; s < steps; ++s) acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
    result[i] = acc;
  });
  for (std::size_t i = 0; i < kCount; ++i) {
    std::uint64_t acc = i;
    const std::size_t steps = i * i / 1000;
    for (std::size_t s = 0; s < steps; ++s) acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
    ASSERT_EQ(result[i], acc) << "index " << i;
  }
}

}  // namespace
