// Shutdown semantics: ~ThreadPool drains. Every task scheduled before the destructor
// started, and everything those tasks spawn, runs before the workers are joined.
#include "stealpool/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "stealpool/task_group.hpp"

namespace {

using stealpool::TaskGroup;
using stealpool::ThreadPool;

TEST(ShutdownTest, IdlePoolsStartAndStopRepeatedly) {
  for (int round = 0; round < 50; ++round) {
    for (std::size_t workers : {1u, 2u, 5u}) {
      ThreadPool pool(workers);
    }
  }
}

TEST(ShutdownTest, DestructorRunsAllPendingExternalTasks) {
  std::atomic<int> ran{0};
  {
    ThreadPool pool(3);
    for (int i = 0; i < 10000; ++i) (void)pool.submit([&] { ran.fetch_add(1, std::memory_order_relaxed); });
  }
  EXPECT_EQ(ran.load(), 10000);
}

// Detached tasks that keep submitting more detached tasks from inside the pool: the whole
// tree must run, although the destructor starts while the tree is still growing.
void SpawnTree(ThreadPool& pool, std::atomic<int>& ran, int depth) {
  ran.fetch_add(1, std::memory_order_relaxed);
  if (depth == 0) return;
  for (int i = 0; i < 3; ++i) (void)pool.submit([&pool, &ran, depth] { SpawnTree(pool, ran, depth - 1); });
}

TEST(ShutdownTest, DestructorRunsTasksSpawnedDuringShutdown) {
  for (std::size_t workers : {1u, 2u, 4u}) {
    std::atomic<int> ran{0};
    {
      ThreadPool pool(workers);
      (void)pool.submit([&] { SpawnTree(pool, ran, 7); });
    }
    EXPECT_EQ(ran.load(), (2187 * 3 - 1) / 2) << workers << " workers";  // 1 + 3 + ... + 3^7
  }
}

// A running task blocks (without helping) until children in its own deque have been
// stolen and run. The destructor starts before the task does, so workers that find
// nothing to do at that moment must not leave while a task is still running.
TEST(ShutdownTest, WorkersStayWhileATaskIsRunning) {
  for (int round = 0; round < 20; ++round) {
    std::atomic<int> children{0};
    {
      ThreadPool pool(3);
      (void)pool.submit([&] {
        std::latch done(8);
        TaskGroup group(pool);
        for (int i = 0; i < 8; ++i) {
          group.spawn([&] {
            children.fetch_add(1);
            done.count_down();
          });
        }
        done.wait();
        group.wait();
      });
    }
    ASSERT_EQ(children.load(), 8) << "round " << round;
  }
}

TEST(ShutdownTest, HandleOutlivesPool) {
  stealpool::TaskHandle<std::string> handle;
  {
    ThreadPool pool(2);
    handle = pool.submit([] { return std::string("still here"); });
  }
  ASSERT_TRUE(handle.valid());
  EXPECT_TRUE(handle.is_ready());
  EXPECT_EQ(handle.get(), "still here");
}

TEST(ShutdownTest, PendingGroupTasksFinishBeforeDestructorReturns) {
  std::atomic<int> ran{0};
  {
    ThreadPool pool(2);
    (void)pool.submit([&] {
      TaskGroup group(pool);
      for (int i = 0; i < 1000; ++i) group.spawn([&] { ran.fetch_add(1); });
      group.wait();
    });
  }
  EXPECT_EQ(ran.load(), 1000);
}

TEST(ShutdownTest, ManyPoolsCreatedAndDestroyedConcurrently) {
  std::atomic<int> ran{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int round = 0; round < 10; ++round) {
        ThreadPool pool(2);
        for (int i = 0; i < 100; ++i) (void)pool.submit([&] { ran.fetch_add(1); });
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(ran.load(), 4 * 10 * 100);
}

TEST(ShutdownTest, CapturesAreReleasedByShutdown) {
  auto token = std::make_shared<int>(0);
  {
    ThreadPool pool(2);
    for (int i = 0; i < 100; ++i) (void)pool.submit([token] { return *token; });
  }
  EXPECT_EQ(token.use_count(), 1);
}

}  // namespace
