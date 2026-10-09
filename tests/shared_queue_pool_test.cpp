#include "stealpool/shared_queue_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "stealpool/parallel_for.hpp"
#include "stealpool/task_group.hpp"
#include "stealpool/thread_pool.hpp"
#include "workloads.hpp"

namespace {

using stealpool::SharedQueuePool;
using stealpool::TaskGroup;
using stealpool::ThreadPool;

// ---- SharedQueuePool on its own ---------------------------------------------------------

TEST(SharedQueuePoolTest, RejectsZeroWorkers) { EXPECT_THROW(SharedQueuePool(0), std::invalid_argument); }

TEST(SharedQueuePoolTest, ReportsConcurrency) {
  SharedQueuePool pool(3);
  EXPECT_EQ(pool.concurrency(), 3u);
  EXPECT_GE(SharedQueuePool::default_concurrency(), 1u);
  SharedQueuePool default_pool;
  EXPECT_EQ(default_pool.concurrency(), SharedQueuePool::default_concurrency());
}

TEST(SharedQueuePoolTest, SubmitReturnsValuesAndMoveOnlyResults) {
  SharedQueuePool pool(2);
  EXPECT_EQ(pool.submit([] { return 6 * 7; }).get(), 42);
  auto owned = std::make_unique<int>(5);
  EXPECT_EQ(pool.submit([p = std::move(owned)] { return *p + 1; }).get(), 6);
  auto ptr = pool.submit([] { return std::make_unique<std::string>("moved"); }).get();
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, "moved");
}

TEST(SharedQueuePoolTest, ExceptionPropagatesThroughHandle) {
  SharedQueuePool pool(2);
  auto handle = pool.submit([]() -> int { throw std::runtime_error("boom"); });
  EXPECT_THROW(handle.get(), std::runtime_error);
  EXPECT_EQ(pool.submit([] { return 1; }).get(), 1);  // the pool keeps working
}

TEST(SharedQueuePoolTest, CurrentWorkerIndex) {
  SharedQueuePool pool(4);
  EXPECT_FALSE(pool.current_worker_index().has_value());
  for (int i = 0; i < 50; ++i) {
    const auto index = pool.submit([&pool] { return pool.current_worker_index(); }).get();
    ASSERT_TRUE(index.has_value());
    EXPECT_LT(*index, 4u);
  }
  // A worker of another pool is not a worker of this one.
  ThreadPool other(1);
  EXPECT_FALSE(other.submit([&pool] { return pool.current_worker_index(); }).get().has_value());
}

TEST(SharedQueuePoolTest, RunsEveryTaskExactlyOnce) {
  constexpr std::size_t kTasks = 20000;
  SharedQueuePool pool(4);
  std::vector<std::atomic<int>> runs(kTasks);
  {
    TaskGroup group(pool);
    for (std::size_t i = 0; i < kTasks; ++i) group.spawn([&runs, i] { runs[i].fetch_add(1); });
    group.wait();
  }
  for (std::size_t i = 0; i < kTasks; ++i) ASSERT_EQ(runs[i].load(), 1) << "task " << i;
  EXPECT_GE(pool.stats().tasks_executed, kTasks);
}

std::uint64_t fib(stealpool::Executor& executor, unsigned n) {
  if (n < 2) return n;
  std::uint64_t a = 0;
  TaskGroup group(executor);
  group.spawn([&] { a = fib(executor, n - 1); });
  const std::uint64_t b = fib(executor, n - 2);
  group.wait();
  return a + b;
}

class SharedQueuePoolWorkers : public ::testing::TestWithParam<std::size_t> {};

// Helping waits make nested fork-join complete even on a single worker.
TEST_P(SharedQueuePoolWorkers, NestedForkJoinDoesNotDeadlock) {
  SharedQueuePool pool(GetParam());
  EXPECT_EQ(pool.submit([&pool] { return fib(pool, 20); }).get(), 6765u);
}

TEST_P(SharedQueuePoolWorkers, ParallelForCoversRange) {
  SharedQueuePool pool(GetParam());
  constexpr std::size_t kCount = 10000;
  std::vector<std::atomic<int>> hits(kCount);
  stealpool::parallel_for(pool, 0, kCount, 7, [&](std::size_t i) { hits[i].fetch_add(1); });
  for (std::size_t i = 0; i < kCount; ++i) ASSERT_EQ(hits[i].load(), 1) << "index " << i;
}

INSTANTIATE_TEST_SUITE_P(WorkerCounts, SharedQueuePoolWorkers, ::testing::Values(1u, 2u, 7u));

TEST(SharedQueuePoolTest, TaskGroupRethrowsFirstExceptionAndRunsTheRest) {
  SharedQueuePool pool(3);
  std::atomic<int> ran{0};
  TaskGroup group(pool);
  for (int i = 0; i < 100; ++i) {
    group.spawn([&ran, i] {
      ran.fetch_add(1);
      if (i % 10 == 0) throw std::runtime_error("task failed");
    });
  }
  EXPECT_THROW(group.wait(), std::runtime_error);
  EXPECT_EQ(ran.load(), 100);
  group.wait();  // the error was consumed; the group is reusable
}

TEST(SharedQueuePoolTest, WaitFromExternalThreads) {
  SharedQueuePool pool(2);
  std::latch release(1);
  auto blocked = pool.submit([&release] {
    release.wait();
    return 9;
  });
  std::thread waiter([&] { EXPECT_EQ(blocked.get(), 9); });
  release.count_down();
  waiter.join();
}

TEST(SharedQueuePoolTest, ConcurrentExternalSubmitters) {
  constexpr int kThreads = 6;
  constexpr int kPerThread = 2000;
  SharedQueuePool pool(3);
  std::atomic<int> total{0};
  std::latch start(kThreads);
  std::vector<std::thread> submitters;
  for (int t = 0; t < kThreads; ++t) {
    submitters.emplace_back([&] {
      start.arrive_and_wait();
      std::vector<stealpool::TaskHandle<int>> handles;
      handles.reserve(kPerThread);
      for (int i = 0; i < kPerThread; ++i) handles.push_back(pool.submit([&total] {
          total.fetch_add(1);
          return 1;
        }));
      int sum = 0;
      for (auto& h : handles) sum += h.get();
      EXPECT_EQ(sum, kPerThread);
    });
  }
  for (auto& t : submitters) t.join();
  EXPECT_EQ(total.load(), kThreads * kPerThread);
}

// Waiting on an event of one pool from a worker of the other must block, not help.
TEST(SharedQueuePoolTest, CrossPoolWaitsInBothDirections) {
  SharedQueuePool shared(2);
  ThreadPool stealing(2);
  const auto a = stealing.submit([&] { return shared.submit([&] { return fib(shared, 15); }).get(); }).get();
  const auto b = shared.submit([&] { return stealing.submit([&] { return fib(stealing, 15); }).get(); }).get();
  EXPECT_EQ(a, 610u);
  EXPECT_EQ(b, 610u);
}

// Destruction drains: tasks scheduled before the destructor, and the tasks they spawn,
// all run before the destructor returns.
TEST(SharedQueuePoolTest, DestructorDrainsPendingAndSpawnedWork) {
  std::atomic<int> ran{0};
  std::latch gate(1);
  {
    SharedQueuePool pool(2);
    for (int i = 0; i < 200; ++i) {
      (void)pool.submit([&ran, &gate, &pool] {
        gate.wait();
        // Detached nested work spawned while the destructor may already be draining.
        (void)pool.submit([&ran] { ran.fetch_add(1); });
        ran.fetch_add(1);
      });
    }
    gate.count_down();
  }
  EXPECT_EQ(ran.load(), 400);
}

TEST(SharedQueuePoolTest, IdlePoolShutsDownRepeatedly) {
  for (int i = 0; i < 50; ++i) {
    SharedQueuePool pool(static_cast<std::size_t>(1 + i % 4));
    if (i % 2 == 0) {
      EXPECT_EQ(pool.submit([] { return 1; }).get(), 1);
    }
  }
}

// Workers that are parked in helping waits and in the main loop must all be woken by new
// work: a chain where each task waits for the next forces nested waits on every worker.
TEST(SharedQueuePoolTest, DeepWaitChain) {
  SharedQueuePool pool(3);
  std::function<int(int)> chain = [&](int depth) -> int {
    if (depth == 0) return 0;
    return pool.submit([&chain, depth] { return chain(depth - 1); }).get() + 1;
  };
  EXPECT_EQ(pool.submit([&] { return chain(200); }).get(), 200);
}

// A worker runs its own newest queued task first (LIFO, like a deque owner) and otherwise
// the oldest task in the queue (FIFO, like a thief). One worker makes the order exact.
TEST(SharedQueuePoolTest, OwnTasksRunNewestFirstOthersOldestFirst) {
  SharedQueuePool pool(1);
  std::vector<int> order;  // written only by the single worker

  // External submissions while the worker is busy: taken oldest first.
  std::latch gate(1);
  auto blocker = pool.submit([&gate] { gate.wait(); });
  std::vector<stealpool::TaskHandle<void>> handles;
  for (int i = 0; i < 5; ++i) handles.push_back(pool.submit([&order, i] { order.push_back(i); }));
  gate.count_down();
  blocker.get();
  for (auto& h : handles) h.get();
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));

  // Children spawned by the worker itself: its helping wait takes the newest first.
  order.clear();
  pool.submit([&] {
    TaskGroup group(pool);
    for (int i = 0; i < 5; ++i) group.spawn([&order, i] { order.push_back(i); });
    group.wait();
  }).get();
  EXPECT_EQ(order, (std::vector<int>{4, 3, 2, 1, 0}));
}

// ---- Benchmark workloads on both pools --------------------------------------------------

template <class Pool>
class WorkloadTest : public ::testing::Test {};
using PoolTypes = ::testing::Types<ThreadPool, SharedQueuePool>;
TYPED_TEST_SUITE(WorkloadTest, PoolTypes);

TYPED_TEST(WorkloadTest, ParallelSortMatchesStdSort) {
  // Small cutoffs so even a test-sized input exercises deep recursion and parallel merges.
  const stealpool::bench::SortParams params{.sort_cutoff = 64, .merge_cutoff = 64};
  for (std::size_t count : {0u, 1u, 63u, 65u, 1000u, 50000u}) {
    TypeParam pool(4);
    std::vector<int> values = stealpool::bench::random_ints(count, count + 1);
    // Many duplicates exercise the binary-search split of equal keys.
    if (count > 1000) {
      for (int& v : values) v %= 97;
    }
    std::vector<int> expected = values;
    std::sort(expected.begin(), expected.end());
    std::vector<int> scratch;
    pool.submit([&] { stealpool::bench::parallel_sort(pool, values, scratch, params); }).get();
    ASSERT_EQ(values, expected) << "count " << count;
  }
}

TYPED_TEST(WorkloadTest, TreeReductionMatchesSequential) {
  const auto tree = stealpool::bench::build_unbalanced_tree(30000, 11);
  ASSERT_EQ(tree.nodes.size(), 30000u);
  EXPECT_EQ(tree.nodes[0].size, 30000u);
  // Skewed splits make the tree far deeper than a balanced one (about 15 levels).
  EXPECT_GT(tree.depth, 30u);
  const stealpool::bench::TreeParams params{.work_rounds = 4, .sequential_cutoff = 16};
  const std::uint64_t expected = stealpool::bench::reduce_tree_sequential(tree, 0, params.work_rounds);
  for (std::size_t threads : {1u, 3u, 8u}) {
    TypeParam pool(threads);
    const auto total = pool.submit([&] { return stealpool::bench::reduce_tree(pool, tree, 0, params); }).get();
    EXPECT_EQ(total, expected) << threads << " threads";
  }
}

TYPED_TEST(WorkloadTest, TreeShapeIsConsistent) {
  const auto tree = stealpool::bench::build_unbalanced_tree(5000, 3);
  // Every node's size is one plus its children's sizes, and preorder layout holds.
  for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
    const auto& n = tree.nodes[i];
    std::uint32_t children = 0;
    if (n.left >= 0) {
      EXPECT_EQ(static_cast<std::size_t>(n.left), i + 1);
      children += tree.nodes[static_cast<std::size_t>(n.left)].size;
    }
    if (n.right >= 0) children += tree.nodes[static_cast<std::size_t>(n.right)].size;
    ASSERT_EQ(n.size, children + 1) << "node " << i;
  }
}

TYPED_TEST(WorkloadTest, SpawnTreeRunsEveryTask) {
  for (std::size_t threads : {1u, 4u}) {
    TypeParam pool(threads);
    EXPECT_EQ(pool.submit([&] { return stealpool::bench::spawn_tree(pool, 12); }).get(), (1u << 12) - 1);
  }
}

}  // namespace
