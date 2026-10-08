#include "stealpool/task_group.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <latch>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "stealpool/thread_pool.hpp"

namespace {

using stealpool::TaskGroup;
using stealpool::ThreadPool;

std::uint64_t GroupFib(stealpool::Executor& executor, unsigned n) {
  if (n < 2) return n;
  std::uint64_t left = 0;
  TaskGroup group(executor);
  group.spawn([&] { left = GroupFib(executor, n - 1); });
  const std::uint64_t right = GroupFib(executor, n - 2);
  group.wait();
  return left + right;
}

// Full binary tree of the given depth, one task per node: returns the node count.
std::uint64_t CountTreeNodes(stealpool::Executor& executor, int depth) {
  if (depth == 0) return 1;
  std::uint64_t left = 0;
  std::uint64_t right = 0;
  TaskGroup group(executor);
  group.spawn([&] { left = CountTreeNodes(executor, depth - 1); });
  group.spawn([&] { right = CountTreeNodes(executor, depth - 1); });
  group.wait();
  return 1 + left + right;
}

void Quicksort(stealpool::Executor& executor, std::vector<int>::iterator first, std::vector<int>::iterator last) {
  if (last - first < 64) {
    std::sort(first, last);
    return;
  }
  const int pivot = *(first + (last - first) / 2);
  auto lower = std::partition(first, last, [pivot](int x) { return x < pivot; });
  auto upper = std::partition(lower, last, [pivot](int x) { return x == pivot; });
  TaskGroup group(executor);
  group.spawn([&executor, first, lower] { Quicksort(executor, first, lower); });
  Quicksort(executor, upper, last);
  group.wait();
}

TEST(TaskGroupTest, EmptyGroupWaitReturnsImmediately) {
  ThreadPool pool(2);
  TaskGroup group(pool);
  group.wait();
  group.wait();
  EXPECT_EQ(&group.executor(), &pool);
}

TEST(TaskGroupTest, RunsAllSpawnedTasksFromExternalThread) {
  ThreadPool pool(4);
  constexpr int kTasks = 5000;
  std::vector<std::atomic<int>> runs(kTasks);
  TaskGroup group(pool);
  for (int i = 0; i < kTasks; ++i) group.spawn([&runs, i] { runs[i].fetch_add(1); });
  group.wait();  // the main thread is not a worker: it blocks
  for (int i = 0; i < kTasks; ++i) ASSERT_EQ(runs[i].load(), 1) << "task " << i;
}

TEST(TaskGroupTest, WaitFromAnotherExternalThread) {
  ThreadPool pool(2);
  std::atomic<int> sum{0};
  std::thread external([&] {
    TaskGroup group(pool);
    for (int i = 1; i <= 100; ++i) group.spawn([&sum, i] { sum.fetch_add(i); });
    group.wait();
  });
  external.join();
  EXPECT_EQ(sum.load(), 5050);
}

TEST(TaskGroupTest, GroupIsReusableAfterWait) {
  ThreadPool pool(3);
  TaskGroup group(pool);
  std::atomic<int> count{0};
  for (int round = 1; round <= 20; ++round) {
    for (int i = 0; i < 50; ++i) group.spawn([&] { count.fetch_add(1); });
    group.wait();
    ASSERT_EQ(count.load(), round * 50);
  }
}

TEST(TaskGroupTest, ChildrenMaySpawnIntoTheirOwnGroup) {
  ThreadPool pool(4);
  std::atomic<int> count{0};
  TaskGroup group(pool);
  for (int i = 0; i < 10; ++i) {
    group.spawn([&] {
      count.fetch_add(1);
      for (int j = 0; j < 10; ++j) {
        group.spawn([&] { count.fetch_add(1); });  // keeps the group open while running
      }
    });
  }
  group.wait();
  EXPECT_EQ(count.load(), 110);
}

class NestedForkJoinTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(NestedForkJoinTest, RecursiveFibonacciCompletes) {
  ThreadPool pool(GetParam());
  // No sequential cutoff: about 28k tasks, each parent waiting on a child that may have
  // been stolen. With a single worker this only terminates because wait() helps.
  EXPECT_EQ(pool.submit([&] { return GroupFib(pool, 22); }).get(), 17711u);
}

TEST_P(NestedForkJoinTest, RecursiveFibonacciFromExternalWait) {
  ThreadPool pool(GetParam());
  EXPECT_EQ(GroupFib(pool, 18), 2584u);  // the outermost wait blocks the main thread
}

TEST_P(NestedForkJoinTest, TreeRecursionCompletes) {
  ThreadPool pool(GetParam());
  EXPECT_EQ(pool.submit([&] { return CountTreeNodes(pool, 13); }).get(), (1u << 14) - 1);
}

TEST_P(NestedForkJoinTest, ParallelQuicksortSorts) {
  ThreadPool pool(GetParam());
  std::vector<int> data(100000);
  std::mt19937 rng(99);
  for (int& x : data) x = static_cast<int>(rng() % 5000);  // many duplicates
  std::vector<int> expected = data;
  std::sort(expected.begin(), expected.end());
  pool.submit([&] { Quicksort(pool, data.begin(), data.end()); }).get();
  EXPECT_EQ(data, expected);
}

TEST_P(NestedForkJoinTest, DeepChainOfNestedGroups) {
  ThreadPool pool(GetParam());
  // Each level waits on a group whose only task opens the next level.
  struct Chain {
    static int Run(stealpool::Executor& executor, int depth) {
      if (depth == 0) return 0;
      int below = 0;
      TaskGroup group(executor);
      group.spawn([&] { below = Run(executor, depth - 1); });
      group.wait();
      return below + 1;
    }
  };
  EXPECT_EQ(pool.submit([&] { return Chain::Run(pool, 300); }).get(), 300);
}

INSTANTIATE_TEST_SUITE_P(Workers, NestedForkJoinTest, ::testing::Values(1, 2, 3, 8),
                         [](const auto& info) { return std::to_string(info.param) + "Workers"; });

TEST(TaskGroupTest, FirstExceptionIsRethrownAndOtherTasksStillRun) {
  ThreadPool pool(4);
  std::atomic<int> ran{0};
  TaskGroup group(pool);
  for (int i = 0; i < 100; ++i) {
    group.spawn([&ran, i] {
      ran.fetch_add(1);
      if (i % 10 == 0) throw std::runtime_error("task " + std::to_string(i));
    });
  }
  try {
    group.wait();
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_EQ(std::string(e.what()).rfind("task ", 0), 0u);
  }
  EXPECT_EQ(ran.load(), 100);
  // The exception was consumed: the group is clean and reusable.
  group.spawn([&] { ran.fetch_add(1); });
  EXPECT_NO_THROW(group.wait());
  EXPECT_EQ(ran.load(), 101);
}

TEST(TaskGroupTest, ExceptionPropagatesThroughNestedGroups) {
  ThreadPool pool(3);
  struct Nested {
    static void Run(stealpool::Executor& executor, int depth) {
      TaskGroup group(executor);
      group.spawn([&executor, depth] {
        if (depth == 0) throw std::out_of_range("leaf");
        Run(executor, depth - 1);
      });
      group.wait();
    }
  };
  auto handle = pool.submit([&] { Nested::Run(pool, 20); });
  EXPECT_THROW(handle.get(), std::out_of_range);
}

TEST(TaskGroupTest, ExceptionOnSingleWorker) {
  ThreadPool pool(1);
  auto handle = pool.submit([&] {
    TaskGroup group(pool);
    group.spawn([] { throw std::invalid_argument("bad"); });
    group.wait();
  });
  EXPECT_THROW(handle.get(), std::invalid_argument);
}

TEST(TaskGroupTest, DestructorWaitsAndDiscardsException) {
  ThreadPool pool(2);
  std::atomic<int> ran{0};
  {
    TaskGroup group(pool);
    for (int i = 0; i < 50; ++i) {
      group.spawn([&] {
        ran.fetch_add(1);
        throw std::runtime_error("ignored");
      });
    }
  }  // no wait(): the destructor joins and must not throw
  EXPECT_EQ(ran.load(), 50);
}

TEST(TaskGroupTest, DestructorWaitsWhenOwnerThrows) {
  ThreadPool pool(2);
  std::atomic<int> ran{0};
  try {
    TaskGroup group(pool);
    for (int i = 0; i < 20; ++i) group.spawn([&] { ran.fetch_add(1); });
    throw std::runtime_error("owner failure");
  } catch (const std::runtime_error&) {
  }
  EXPECT_EQ(ran.load(), 20);
}

// The waiting worker finds nothing to run while its child is still busy elsewhere, so it
// has to park and be woken by the child's completion (a targeted wake, not new work).
// Spins (yielding) until `pred` holds. Used to order threads without sleeps.
template <class Pred>
void SpinUntil(Pred pred) {
  while (!pred()) std::this_thread::yield();
}

TEST(TaskGroupTest, WaitingWorkerIsWokenByCompletion) {
  // The parent's worker must really park in group.wait() and be woken by the child's
  // completion: the child, running on the other worker, finishes only after the pool has
  // counted a new park, and with two workers only the parent's worker can be parking.
  ThreadPool pool(2);
  for (int round = 0; round < 50; ++round) {
    std::latch child_started(1);
    auto parent = pool.submit([&] {
      TaskGroup group(pool);
      group.spawn([&] {
        const std::uint64_t parks_before = pool.stats().parks;
        child_started.count_down();
        SpinUntil([&] { return pool.stats().parks > parks_before; });
      });
      // Blocking (not helping) here forces the other worker to steal the child.
      child_started.wait();
      group.wait();
      return 1;
    });
    ASSERT_EQ(parent.get(), 1);
  }
}

// Regression test for a lost wakeup. A helping waiter (Wa) is woken by its completed event
// while it is still on the idle list; a producer (Wp) then submits D and its single wakeup
// pops Wa instead of the really parked worker Wq. Before the fix Wa left its wait without
// looking for D, Wq kept sleeping and D was never run: this test hung. Roles are set up
// with atomics and stats().parks only:
//   * P (on Wp) spawns h1 into group H; h1 is stolen by Wq and holds it.
//   * A (on Wa, the only free worker) spawns c into group G and blocks until c starts.
//   * P enters H.wait() and, helping, steals c. c lets h1 finish, waits for Wq to park,
//     lets A enter G.wait(), waits for Wa to park (so Wa is the newest idle worker), and
//     returns, which unparks Wa.
//   * P, back from H.wait(), submits D and blocks until D has run; so does A.
TEST(TaskGroupTest, WakeupSpentOnAwakeWaiterIsNotLost) {
  ThreadPool pool(3);
  for (int round = 0; round < 20; ++round) {
    std::atomic<bool> h1_started{false}, h1_go{false}, a_spawned{false}, c_started{false}, a_go{false},
        d_ran{false};
    auto p = pool.submit([&] {
      TaskGroup h(pool);
      h.spawn([&] {
        h1_started.store(true);
        SpinUntil([&] { return h1_go.load(); });
      });
      SpinUntil([&] { return a_spawned.load(); });
      h.wait();  // helping: steals c from A's worker
      auto d = pool.submit([&] { d_ran.store(true); });
      SpinUntil([&] { return d_ran.load(); });
      d.get();
      return 1;
    });
    SpinUntil([&] { return h1_started.load(); });
    auto a = pool.submit([&] {
      TaskGroup g(pool);
      g.spawn([&] {
        const std::uint64_t parks_before = pool.stats().parks;
        c_started.store(true);
        h1_go.store(true);
        SpinUntil([&] { return pool.stats().parks >= parks_before + 1; });  // Wq parked
        a_go.store(true);
        SpinUntil([&] { return pool.stats().parks >= parks_before + 2; });  // Wa parked
      });
      a_spawned.store(true);
      SpinUntil([&] { return c_started.load() && a_go.load(); });
      g.wait();
      SpinUntil([&] { return d_ran.load(); });
      return 2;
    });
    ASSERT_EQ(p.get(), 1);
    ASSERT_EQ(a.get(), 2);
  }
}

TEST(TaskGroupTest, CapturedStateIsReleasedBeforeWaitReturns) {
  ThreadPool pool(2);
  auto token = std::make_shared<int>(1);
  TaskGroup group(pool);
  for (int i = 0; i < 10; ++i) group.spawn([token] { (void)*token; });
  group.wait();
  EXPECT_EQ(token.use_count(), 1);
}

}  // namespace
