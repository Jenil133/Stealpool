#include "stealpool/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "stealpool/task_group.hpp"

namespace {

using stealpool::TaskHandle;
using stealpool::ThreadPool;

TEST(ThreadPoolTest, RejectsZeroWorkers) { EXPECT_THROW(ThreadPool(0), std::invalid_argument); }

TEST(ThreadPoolTest, ReportsConcurrency) {
  ThreadPool pool(3);
  EXPECT_EQ(pool.concurrency(), 3u);
  EXPECT_GE(ThreadPool::default_concurrency(), 1u);
  ThreadPool default_pool;
  EXPECT_EQ(default_pool.concurrency(), ThreadPool::default_concurrency());
}

TEST(ThreadPoolTest, SubmitReturnsValue) {
  ThreadPool pool(2);
  auto handle = pool.submit([] { return 6 * 7; });
  EXPECT_TRUE(handle.valid());
  EXPECT_EQ(handle.get(), 42);
  EXPECT_FALSE(handle.valid());  // get() consumes the handle, like std::future
}

TEST(ThreadPoolTest, SubmitVoidAndMoveOnlyResults) {
  ThreadPool pool(2);
  std::atomic<int> ran{0};
  auto void_handle = pool.submit([&] { ran.fetch_add(1); });
  void_handle.get();
  EXPECT_EQ(ran.load(), 1);

  auto ptr_handle = pool.submit([] { return std::make_unique<std::string>("moved"); });
  std::unique_ptr<std::string> result = ptr_handle.get();
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(*result, "moved");
}

TEST(ThreadPoolTest, MoveOnlyCallable) {
  ThreadPool pool(1);
  auto owned = std::make_unique<int>(5);
  auto handle = pool.submit([p = std::move(owned)] { return *p + 1; });
  EXPECT_EQ(handle.get(), 6);
}

TEST(ThreadPoolTest, HandleIsMovable) {
  ThreadPool pool(2);
  TaskHandle<int> a = pool.submit([] { return 1; });
  TaskHandle<int> b = std::move(a);
  EXPECT_FALSE(a.valid());  // checking the moved-from state on purpose
  ASSERT_TRUE(b.valid());
  TaskHandle<int> c;
  EXPECT_FALSE(c.valid());
  c = std::move(b);
  EXPECT_EQ(c.get(), 1);
}

TEST(ThreadPoolTest, WaitThenIsReady) {
  ThreadPool pool(2);
  auto handle = pool.submit([] { return 3; });
  handle.wait();
  EXPECT_TRUE(handle.is_ready());
  handle.wait();  // waiting again on a ready handle returns immediately
  EXPECT_EQ(handle.get(), 3);
}

TEST(ThreadPoolTest, ExceptionPropagatesThroughGet) {
  ThreadPool pool(2);
  auto handle = pool.submit([]() -> int { throw std::runtime_error("task failed"); });
  try {
    (void)handle.get();
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ(e.what(), "task failed");
  }
  EXPECT_FALSE(handle.valid());
  // The pool is still usable afterwards.
  EXPECT_EQ(pool.submit([] { return 1; }).get(), 1);
}

TEST(ThreadPoolTest, ExceptionPropagatesForVoidTasks) {
  ThreadPool pool(1);
  auto handle = pool.submit([] { throw std::logic_error("void failure"); });
  EXPECT_THROW(handle.get(), std::logic_error);
}

TEST(ThreadPoolTest, CallableIsDestroyedBeforeGetReturns) {
  ThreadPool pool(2);
  auto token = std::make_shared<int>(0);
  auto handle = pool.submit([token] { return *token; });
  EXPECT_EQ(handle.get(), 0);
  EXPECT_EQ(token.use_count(), 1);
}

TEST(ThreadPoolTest, CurrentWorkerIndex) {
  ThreadPool pool(3);
  EXPECT_FALSE(pool.current_worker_index().has_value());
  auto index = pool.submit([&] { return pool.current_worker_index(); }).get();
  ASSERT_TRUE(index.has_value());
  EXPECT_LT(*index, 3u);

  // A worker of another pool is an external thread for this one.
  ThreadPool other(1);
  auto foreign = other.submit([&] { return pool.current_worker_index(); }).get();
  EXPECT_FALSE(foreign.has_value());
}

class PoolSizeTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(PoolSizeTest, RunsEveryTaskExactlyOnce) {
  ThreadPool pool(GetParam());
  constexpr std::size_t kTasks = 20000;
  std::vector<std::atomic<int>> runs(kTasks);
  std::vector<TaskHandle<void>> handles;
  handles.reserve(kTasks);
  for (std::size_t i = 0; i < kTasks; ++i) {
    handles.push_back(pool.submit([&runs, i] { runs[i].fetch_add(1, std::memory_order_relaxed); }));
  }
  for (auto& h : handles) h.get();
  for (std::size_t i = 0; i < kTasks; ++i) ASSERT_EQ(runs[i].load(), 1) << "task " << i;
  EXPECT_GE(pool.stats().tasks_executed, kTasks);
}

TEST_P(PoolSizeTest, TasksSubmittedFromWorkersRunExactlyOnce) {
  ThreadPool pool(GetParam());
  constexpr std::size_t kParents = 64;
  constexpr std::size_t kChildren = 100;
  std::vector<std::atomic<int>> runs(kParents * kChildren);
  auto root = pool.submit([&] {
    std::vector<TaskHandle<void>> parents;
    for (std::size_t p = 0; p < kParents; ++p) {
      parents.push_back(pool.submit([&, p] {
        std::vector<TaskHandle<void>> children;
        for (std::size_t c = 0; c < kChildren; ++c) {
          children.push_back(pool.submit([&, p, c] { runs[p * kChildren + c].fetch_add(1); }));
        }
        for (auto& h : children) h.get();  // helping wait inside a worker
      }));
    }
    for (auto& h : parents) h.get();
  });
  root.get();
  for (std::size_t i = 0; i < runs.size(); ++i) ASSERT_EQ(runs[i].load(), 1) << "task " << i;
}

std::uint64_t SubmitFib(ThreadPool& pool, unsigned n) {
  if (n < 2) return n;
  auto left = pool.submit([&pool, n] { return SubmitFib(pool, n - 1); });
  const std::uint64_t right = SubmitFib(pool, n - 2);
  return left.get() + right;
}

TEST_P(PoolSizeTest, RecursiveSubmitWithHelpingGet) {
  ThreadPool pool(GetParam());
  EXPECT_EQ(pool.submit([&] { return SubmitFib(pool, 18); }).get(), 2584u);
}

INSTANTIATE_TEST_SUITE_P(Workers, PoolSizeTest, ::testing::Values(1, 2, 4, 8),
                         [](const auto& info) { return std::to_string(info.param) + "Workers"; });

TEST(ThreadPoolTest, ConcurrentExternalSubmitters) {
  ThreadPool pool(4);
  constexpr int kSubmitters = 6;
  constexpr int kPerSubmitter = 4000;
  std::vector<std::atomic<int>> runs(kSubmitters * kPerSubmitter);
  std::latch start(kSubmitters);
  std::vector<std::thread> submitters;
  for (int s = 0; s < kSubmitters; ++s) {
    submitters.emplace_back([&, s] {
      start.arrive_and_wait();  // maximise contention on the injection queue
      std::vector<TaskHandle<void>> handles;
      handles.reserve(kPerSubmitter);
      for (int i = 0; i < kPerSubmitter; ++i) {
        handles.push_back(pool.submit([&runs, idx = s * kPerSubmitter + i] { runs[idx].fetch_add(1); }));
      }
      for (auto& h : handles) h.get();  // external threads block in get()
    });
  }
  for (auto& t : submitters) t.join();
  for (std::size_t i = 0; i < runs.size(); ++i) ASSERT_EQ(runs[i].load(), 1) << "task " << i;
}

TEST(ThreadPoolTest, WaitOnHandleFromAnotherExternalThread) {
  ThreadPool pool(2);
  std::latch release(1);
  auto handle = pool.submit([&] {
    release.wait();
    return 17;
  });
  std::optional<int> seen;
  std::thread waiter([&] { seen = handle.get(); });
  release.count_down();
  waiter.join();
  EXPECT_EQ(seen, std::optional<int>(17));
}

// A task that blocks (without helping) after spawning children can only finish if other
// workers steal those children from its deque. No timing involved: if stealing were
// broken this test would hang (and hit the CTest timeout) rather than pass by luck.
TEST(ThreadPoolTest, IdleWorkersStealFromABlockedWorker) {
  ThreadPool pool(4);
  constexpr int kChildren = 64;
  std::atomic<int> ran{0};
  pool.submit([&] {
        std::latch children_done(kChildren);
        stealpool::TaskGroup group(pool);
        for (int i = 0; i < kChildren; ++i) {
          group.spawn([&] {
            ran.fetch_add(1);
            children_done.count_down();
          });
        }
        children_done.wait();  // blocking, not helping: the children must be stolen
        group.wait();
      })
      .get();
  EXPECT_EQ(ran.load(), kChildren);
  EXPECT_GE(pool.stats().steals, static_cast<std::uint64_t>(kChildren));
}

TEST(ThreadPoolTest, WorkerWaitingOnAnotherPoolBlocks) {
  ThreadPool a(1);
  ThreadPool b(1);
  // A worker of `a` waits on a task of `b`: that is an external wait for `b`, so it blocks
  // instead of trying to run `b`'s tasks.
  const int value = a.submit([&] { return b.submit([] { return 9; }).get(); }).get();
  EXPECT_EQ(value, 9);
}

TEST(ThreadPoolTest, DroppedHandleStillRuns) {
  std::atomic<int> ran{0};
  {
    ThreadPool pool(2);
    for (int i = 0; i < 100; ++i) {
      (void)pool.submit([&] { ran.fetch_add(1); });
    }
  }  // the destructor drains
  EXPECT_EQ(ran.load(), 100);
}

TEST(ThreadPoolTest, StatsCountExecutedTasks) {
  ThreadPool pool(2);
  for (int i = 0; i < 50; ++i) pool.submit([] {}).get();
  const auto stats = pool.stats();
  EXPECT_EQ(stats.tasks_executed, 50u);
  // Submitted from outside: every task goes through the injection queue, none is stolen.
  EXPECT_EQ(stats.injected, 50u);
  EXPECT_EQ(stats.steals, 0u);
}

}  // namespace
