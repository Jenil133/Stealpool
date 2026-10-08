#include "stealpool/deque.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace {

using stealpool::WorkStealingDeque;

TEST(DequeTest, EmptyDequeReturnsNothing) {
  WorkStealingDeque<int> dq;
  EXPECT_FALSE(dq.pop().has_value());
  EXPECT_FALSE(dq.steal().has_value());
  EXPECT_TRUE(dq.empty_approx());
  // Failed pops must leave the indices consistent.
  for (int i = 0; i < 10; ++i) EXPECT_FALSE(dq.pop().has_value());
  dq.push(7);
  EXPECT_EQ(dq.size_approx(), 1u);
  EXPECT_EQ(dq.pop(), std::optional<int>(7));
  EXPECT_FALSE(dq.pop().has_value());
}

TEST(DequeTest, TryStealReportsEmpty) {
  WorkStealingDeque<int> dq;
  int out = -1;
  EXPECT_EQ(dq.try_steal(out), WorkStealingDeque<int>::StealStatus::kEmpty);
  EXPECT_EQ(out, -1);
  dq.push(3);
  EXPECT_EQ(dq.try_steal(out), WorkStealingDeque<int>::StealStatus::kSuccess);
  EXPECT_EQ(out, 3);
}

TEST(DequeTest, OwnerPopIsLifo) {
  WorkStealingDeque<int> dq;
  for (int i = 0; i < 100; ++i) dq.push(i);
  for (int i = 99; i >= 0; --i) EXPECT_EQ(dq.pop(), std::optional<int>(i));
  EXPECT_FALSE(dq.pop().has_value());
}

TEST(DequeTest, StealIsFifo) {
  WorkStealingDeque<int> dq;
  for (int i = 0; i < 100; ++i) dq.push(i);
  for (int i = 0; i < 100; ++i) EXPECT_EQ(dq.steal(), std::optional<int>(i));
  EXPECT_FALSE(dq.steal().has_value());
}

TEST(DequeTest, PopAndStealTakeOppositeEnds) {
  WorkStealingDeque<int> dq;
  dq.push(1);
  dq.push(2);
  dq.push(3);
  EXPECT_EQ(dq.steal(), std::optional<int>(1));
  EXPECT_EQ(dq.pop(), std::optional<int>(3));
  EXPECT_EQ(dq.steal(), std::optional<int>(2));
  EXPECT_FALSE(dq.pop().has_value());
  EXPECT_FALSE(dq.steal().has_value());
}

TEST(DequeTest, CapacityIsRoundedToPowerOfTwo) {
  EXPECT_EQ(WorkStealingDeque<int>(0).capacity(), 2u);
  EXPECT_EQ(WorkStealingDeque<int>(3).capacity(), 4u);
  EXPECT_EQ(WorkStealingDeque<int>(64).capacity(), 64u);
  EXPECT_EQ(WorkStealingDeque<int>(65).capacity(), 128u);
}

TEST(DequeTest, GrowsAcrossManyResizes) {
  WorkStealingDeque<std::int64_t> dq(2);
  constexpr std::int64_t kCount = 1 << 17;  // 16 doublings from capacity 2
  for (std::int64_t i = 0; i < kCount; ++i) dq.push(i);
  EXPECT_GE(dq.capacity(), static_cast<std::size_t>(kCount));
  EXPECT_EQ(dq.size_approx(), static_cast<std::size_t>(kCount));
  // Steal half from the top, pop the rest from the bottom: both orders survive growth.
  for (std::int64_t i = 0; i < kCount / 2; ++i) ASSERT_EQ(dq.steal(), std::optional<std::int64_t>(i));
  for (std::int64_t i = kCount - 1; i >= kCount / 2; --i) ASSERT_EQ(dq.pop(), std::optional<std::int64_t>(i));
  EXPECT_FALSE(dq.pop().has_value());
}

TEST(DequeTest, GrowthAfterWrapAroundKeepsOrder) {
  // Advance top/bottom so the live range straddles the end of the ring when it grows.
  WorkStealingDeque<int> dq(4);
  for (int round = 0; round < 3; ++round) {
    dq.push(-1);
    dq.push(-1);
    ASSERT_TRUE(dq.steal().has_value());
    ASSERT_TRUE(dq.steal().has_value());
  }
  for (int i = 0; i < 20; ++i) dq.push(i);  // grows 4 -> 8 -> 16 -> 32 with a wrapped range
  for (int i = 0; i < 20; ++i) ASSERT_EQ(dq.steal(), std::optional<int>(i));
}

TEST(DequeTest, RandomSingleThreadedOperationsMatchModel) {
  WorkStealingDeque<std::uint32_t> dq(2);
  std::deque<std::uint32_t> model;
  std::mt19937 rng(12345);
  std::uint32_t next = 0;
  for (int op = 0; op < 200000; ++op) {
    const unsigned choice = rng() % 10;
    if (choice < 5) {
      dq.push(next);
      model.push_back(next);
      ++next;
    } else if (choice < 8) {
      const auto got = dq.pop();
      if (model.empty()) {
        ASSERT_FALSE(got.has_value());
      } else {
        ASSERT_EQ(got, std::optional<std::uint32_t>(model.back()));
        model.pop_back();
      }
    } else {
      const auto got = dq.steal();
      if (model.empty()) {
        ASSERT_FALSE(got.has_value());
      } else {
        ASSERT_EQ(got, std::optional<std::uint32_t>(model.front()));
        model.pop_front();
      }
    }
    ASSERT_EQ(dq.size_approx(), model.size());
  }
}

TEST(DequeTest, StoresPointers) {
  int values[3] = {10, 20, 30};
  WorkStealingDeque<int*> dq;
  for (int& v : values) dq.push(&v);
  EXPECT_EQ(dq.steal(), std::optional<int*>(&values[0]));
  EXPECT_EQ(dq.pop(), std::optional<int*>(&values[2]));
  EXPECT_EQ(dq.pop(), std::optional<int*>(&values[1]));
}

// Owner pop versus thieves' steal on the last element: exactly one side gets it, in every
// round. Rounds are separated by a barrier, never by sleeping.
void RunLastElementRace(int thieves, int rounds) {
  WorkStealingDeque<int> dq;
  std::barrier sync(thieves + 1);
  std::atomic<int> stolen_this_round{0};
  std::atomic<long> total_stolen{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < thieves; ++t) {
    threads.emplace_back([&] {
      for (int r = 0; r < rounds; ++r) {
        sync.arrive_and_wait();  // element pushed
        if (auto v = dq.steal()) {
          EXPECT_EQ(*v, r);
          stolen_this_round.fetch_add(1, std::memory_order_relaxed);
          total_stolen.fetch_add(1, std::memory_order_relaxed);
        }
        sync.arrive_and_wait();  // round finished
      }
    });
  }

  long popped = 0;
  for (int r = 0; r < rounds; ++r) {
    dq.push(r);
    sync.arrive_and_wait();
    const auto mine = dq.pop();
    if (mine) {
      EXPECT_EQ(*mine, r);
      ++popped;
    }
    sync.arrive_and_wait();
    const int stolen = stolen_this_round.exchange(0, std::memory_order_relaxed);
    ASSERT_EQ((mine ? 1 : 0) + stolen, 1) << "round " << r;
    ASSERT_TRUE(dq.empty_approx());
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(popped + total_stolen.load(), rounds);
}

TEST(DequeTest, LastElementOwnerVersusOneThief) { RunLastElementRace(1, 5000); }
TEST(DequeTest, LastElementOwnerVersusThreeThieves) { RunLastElementRace(3, 2000); }

// The owner keeps pushing (forcing growth from capacity 2) and occasionally popping while
// several thieves steal. Every item must be taken exactly once.
void RunConcurrentOwnerAndThieves(int thieves, int items, unsigned seed) {
  WorkStealingDeque<int> dq(2);
  std::vector<std::atomic<int>> taken(static_cast<std::size_t>(items));
  std::atomic<bool> owner_done{false};
  std::atomic<long> steals{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < thieves; ++t) {
    threads.emplace_back([&] {
      long mine = 0;
      for (;;) {
        // Read the flag before stealing: if the owner was done and the deque is then
        // empty, nothing can ever be pushed again.
        const bool done = owner_done.load(std::memory_order_acquire);
        if (auto v = dq.steal()) {
          taken[static_cast<std::size_t>(*v)].fetch_add(1, std::memory_order_relaxed);
          ++mine;
        } else if (done && dq.empty_approx()) {
          break;
        }
      }
      steals.fetch_add(mine, std::memory_order_relaxed);
    });
  }

  std::mt19937 rng(seed);
  long pops = 0;
  for (int i = 0; i < items; ++i) {
    dq.push(i);
    if (rng() % 3 == 0) {
      if (auto v = dq.pop()) {
        taken[static_cast<std::size_t>(*v)].fetch_add(1, std::memory_order_relaxed);
        ++pops;
      }
    }
  }
  while (auto v = dq.pop()) {
    taken[static_cast<std::size_t>(*v)].fetch_add(1, std::memory_order_relaxed);
    ++pops;
  }
  owner_done.store(true, std::memory_order_release);
  for (auto& t : threads) t.join();

  EXPECT_EQ(pops + steals.load(), items);
  for (int i = 0; i < items; ++i) {
    ASSERT_EQ(taken[static_cast<std::size_t>(i)].load(), 1) << "item " << i << " seed " << seed;
  }
}

TEST(DequeTest, ConcurrentOwnerAndOneThief) { RunConcurrentOwnerAndThieves(1, 100000, 1); }
TEST(DequeTest, ConcurrentOwnerAndFourThieves) { RunConcurrentOwnerAndThieves(4, 100000, 2); }

}  // namespace
