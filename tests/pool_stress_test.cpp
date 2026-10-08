// Randomized stress tests for ThreadPool, TaskGroup and parallel_for (label "stress").
//
// All shapes (task trees, group programs, loop costs) are pure functions of a seed, so
// the expected totals are computed sequentially up front and compared with what the
// pool actually ran. Each tree node contributes both a count and a hash, which makes a
// lost task and a duplicated task show up even if they happened in the same run. Seeds
// and operation totals are printed; see stress_util.hpp for STEALPOOL_STRESS_SEED and
// STEALPOOL_STRESS_SCALE.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "stealpool/parallel_for.hpp"
#include "stealpool/task_group.hpp"
#include "stealpool/thread_pool.hpp"
#include "stress_util.hpp"

namespace {

using stealpool::TaskGroup;
using stealpool::TaskHandle;
using stealpool::ThreadPool;
using stress::Mix;
using stress::Rng;
using stress::U;

// ---------------------------------------------------------------------------------------
// Shared bookkeeping

struct PoolTotals {
  std::uint64_t pools = 0;
  std::uint64_t tasks_executed = 0;
  std::uint64_t steals = 0;
  std::uint64_t injected = 0;
  std::uint64_t parks = 0;

  void Add(const ThreadPool::Stats& s) {
    ++pools;
    tasks_executed += s.tasks_executed;
    steals += s.steals;
    injected += s.injected;
    parks += s.parks;
  }

  void Print(const char* what, const std::string& extra) const {
    std::printf("[stress] %s: %s; %llu pools, %llu tasks executed, %llu steals, %llu injected, %llu parks\n", what,
                extra.c_str(), U(pools), U(tasks_executed), U(steals), U(injected), U(parks));
    std::fflush(stdout);
  }
};

// Mixed task sizes: most tasks are tiny, about one in 32 is a few thousand times
// heavier. The heavy ones are what idle workers must steal around.
void BusyWork(std::uint64_t seed) {
  const std::uint64_t steps = (Mix(seed ^ 0xabcdefULL) % 32 == 0) ? 4000 : 4;
  volatile std::uint64_t sink = seed;
  for (std::uint64_t s = 0; s < steps; ++s) sink = sink * 31 + s;
}

std::uint64_t NodeHash(std::uint64_t seed) { return Mix(seed ^ 0x1234567ULL); }

struct Visits {
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> hash{0};  // wrapping sum, independent of completion order

  void Record(std::uint64_t seed) {
    count.fetch_add(1, std::memory_order_relaxed);
    hash.fetch_add(NodeHash(seed), std::memory_order_relaxed);
  }
};

struct Expected {
  std::uint64_t count = 0;
  std::uint64_t hash = 0;
  void Record(std::uint64_t seed) {
    ++count;
    hash += NodeHash(seed);
  }
};

void ExpectVisits(const Visits& got, const Expected& want) {
  EXPECT_EQ(got.count.load(), want.count) << "tasks lost or run twice";
  EXPECT_EQ(got.hash.load(), want.hash) << "the set of tasks that ran differs from the expected set";
}

// ---------------------------------------------------------------------------------------
// Random task trees. Each node picks how it forks its children: a TaskGroup, submit() +
// get(), parallel_for over the children, or plain recursion on the current thread.

int TreeFanout(std::uint64_t seed, int depth) { return depth <= 0 ? 0 : static_cast<int>(Mix(seed) % 5); }
std::uint64_t TreeChild(std::uint64_t seed, int i) { return Mix(seed + static_cast<std::uint64_t>(i) + 1); }

enum class ForkMode { kGroup, kSubmit, kParallelFor, kInline };
ForkMode TreeMode(std::uint64_t seed) { return static_cast<ForkMode>(Mix(seed * 7 + 3) % 4); }

void ExpectTree(Expected& e, std::uint64_t seed, int depth) {
  e.Record(seed);
  const int fanout = TreeFanout(seed, depth);
  for (int i = 0; i < fanout; ++i) ExpectTree(e, TreeChild(seed, i), depth - 1);
}

void RunTree(ThreadPool& pool, Visits& v, std::uint64_t seed, int depth) {
  v.Record(seed);
  BusyWork(seed);
  const int fanout = TreeFanout(seed, depth);
  if (fanout == 0) return;
  switch (TreeMode(seed)) {
    case ForkMode::kGroup: {
      TaskGroup group(pool);
      for (int i = 0; i < fanout; ++i) {
        const std::uint64_t child = TreeChild(seed, i);
        group.spawn([&pool, &v, child, depth] { RunTree(pool, v, child, depth - 1); });
      }
      group.wait();
      break;
    }
    case ForkMode::kSubmit: {
      std::vector<TaskHandle<std::uint64_t>> handles;
      for (int i = 0; i < fanout; ++i) {
        const std::uint64_t child = TreeChild(seed, i);
        handles.push_back(pool.submit([&pool, &v, child, depth] {
          RunTree(pool, v, child, depth - 1);
          return child;
        }));
      }
      // Waiting in reverse order makes the helping wait run unrelated tasks first.
      for (int i = fanout - 1; i >= 0; --i) {
        EXPECT_EQ(handles[static_cast<std::size_t>(i)].get(), TreeChild(seed, i));
      }
      break;
    }
    case ForkMode::kParallelFor:
      stealpool::parallel_for(pool, 0, static_cast<std::size_t>(fanout), 1, [&pool, &v, seed, depth](std::size_t i) {
        RunTree(pool, v, TreeChild(seed, static_cast<int>(i)), depth - 1);
      });
      break;
    case ForkMode::kInline:
      for (int i = 0; i < fanout; ++i) RunTree(pool, v, TreeChild(seed, i), depth - 1);
      break;
  }
}

TEST(PoolStressTest, RandomTaskTreesFromConcurrentSubmitters) {
  stress::PrintHeader("PoolStressTest.RandomTaskTreesFromConcurrentSubmitters");
  constexpr int kDepth = 9;
  const std::uint64_t roots_per_submitter = stress::Scaled(32, 1);
  PoolTotals totals;
  std::uint64_t nodes = 0;
  std::uint64_t index = 0;
  for (std::size_t workers : {1u, 2u, 4u, 8u, 16u}) {
    for (int submitters : {1, 4}) {
      const std::uint64_t seed = stress::DerivedSeed(10, index++);
      SCOPED_TRACE(stress::ReproText(
          seed, "workers " + std::to_string(workers) + ", submitters " + std::to_string(submitters)));
      Expected expected;
      std::vector<std::vector<std::uint64_t>> roots(static_cast<std::size_t>(submitters));
      for (int s = 0; s < submitters; ++s) {
        for (std::uint64_t r = 0; r < roots_per_submitter; ++r) {
          const std::uint64_t root = Mix(seed + static_cast<std::uint64_t>(s) * 1000003 + r);
          roots[static_cast<std::size_t>(s)].push_back(root);
          ExpectTree(expected, root, kDepth);
        }
      }
      Visits visits;
      ThreadPool pool(workers);
      std::vector<std::thread> threads;
      for (int s = 0; s < submitters; ++s) {
        threads.emplace_back([&, s] {
          // Submit every root first, then wait, so trees from all submitters overlap.
          std::vector<TaskHandle<void>> handles;
          for (std::uint64_t root : roots[static_cast<std::size_t>(s)]) {
            handles.push_back(pool.submit([&pool, &visits, root] { RunTree(pool, visits, root, kDepth); }));
          }
          for (auto& h : handles) h.get();
        });
      }
      for (auto& t : threads) t.join();
      ExpectVisits(visits, expected);
      totals.Add(pool.stats());
      nodes += expected.count;
      if (HasFailure()) return;
    }
  }
  totals.Print("random task trees", std::to_string(nodes) + " tree nodes");
}

// ---------------------------------------------------------------------------------------
// TaskGroup programs: a group reused for many rounds; its tasks spawn more tasks into the
// same group while they run; a few tasks throw. Every task must run even when some throw,
// and wait() must rethrow one of the thrown exceptions exactly when something threw.

struct StressError : std::exception {
  explicit StressError(std::uint64_t s) : seed(s) {}
  const char* what() const noexcept override { return "stress task failure"; }
  std::uint64_t seed;
};

int GroupKids(std::uint64_t seed, int depth) { return depth <= 0 ? 0 : static_cast<int>(Mix(seed) % 3); }
bool GroupThrows(std::uint64_t seed) { return Mix(seed ^ 0xbadULL) % 61 == 0; }

void ExpectGroupTask(Expected& e, std::set<std::uint64_t>& throwers, std::uint64_t seed, int depth) {
  e.Record(seed);
  if (GroupThrows(seed)) throwers.insert(seed);
  for (int i = 0; i < GroupKids(seed, depth); ++i) ExpectGroupTask(e, throwers, TreeChild(seed, i), depth - 1);
}

void GroupTask(TaskGroup& group, Visits& v, std::uint64_t seed, int depth) {
  v.Record(seed);
  BusyWork(seed);
  // Spawn into our own group before (possibly) throwing: the children must still run.
  for (int i = 0; i < GroupKids(seed, depth); ++i) {
    const std::uint64_t child = TreeChild(seed, i);
    group.spawn([&group, &v, child, depth] { GroupTask(group, v, child, depth - 1); });
  }
  if (GroupThrows(seed)) throw StressError(seed);
}

// Runs `rounds` rounds on one reused group; returns the number of rounds that threw.
std::uint64_t RunGroupProgram(ThreadPool& pool, std::uint64_t seed, std::uint64_t rounds, Visits& v, Expected& e) {
  constexpr int kDepth = 6;
  TaskGroup group(pool);
  std::uint64_t threw = 0;
  Rng rng(seed);
  for (std::uint64_t round = 0; round < rounds; ++round) {
    std::set<std::uint64_t> throwers;
    const std::uint64_t spawns = 1 + rng.below(16);
    for (std::uint64_t i = 0; i < spawns; ++i) {
      const std::uint64_t task_seed = rng.next();
      ExpectGroupTask(e, throwers, task_seed, kDepth);
      group.spawn([&group, &v, task_seed] { GroupTask(group, v, task_seed, kDepth); });
    }
    std::optional<std::uint64_t> caught;
    try {
      group.wait();
    } catch (const StressError& error) {
      caught = error.seed;
    }
    if (throwers.empty()) {
      EXPECT_FALSE(caught.has_value()) << "round " << round << " rethrew although no task threw";
    } else {
      ++threw;
      EXPECT_TRUE(caught.has_value()) << "round " << round << " swallowed " << throwers.size() << " exceptions";
      if (caught) {
        EXPECT_EQ(throwers.count(*caught), 1u) << "rethrown exception came from a task that did not throw";
      }
    }
  }
  return threw;
}

TEST(PoolStressTest, ReusedGroupsWithSelfSpawningAndExceptions) {
  stress::PrintHeader("PoolStressTest.ReusedGroupsWithSelfSpawningAndExceptions");
  const std::uint64_t rounds = stress::Scaled(150, 2);
  PoolTotals totals;
  std::uint64_t tasks = 0;
  std::uint64_t throwing_rounds = 0;
  std::uint64_t index = 0;
  for (std::size_t workers : {1u, 3u, 8u}) {
    const std::uint64_t seed = stress::DerivedSeed(20, index++);
    SCOPED_TRACE(stress::ReproText(seed, "workers " + std::to_string(workers)));
    // Three group owners on external threads (blocking waits) and three running as pool
    // tasks (helping waits), all at once on the same pool.
    constexpr int kOwners = 6;
    std::vector<Visits> visits(kOwners);
    std::vector<Expected> expected(kOwners);
    std::vector<std::uint64_t> threw(kOwners, 0);
    {
      ThreadPool pool(workers);
      std::vector<std::thread> external;
      std::vector<TaskHandle<void>> internal;
      for (int o = 0; o < kOwners; ++o) {
        const auto k = static_cast<std::size_t>(o);
        const std::uint64_t owner_seed = Mix(seed + k);
        auto program = [&pool, &visits, &expected, &threw, k, owner_seed, rounds] {
          threw[k] = RunGroupProgram(pool, owner_seed, rounds, visits[k], expected[k]);
        };
        if (o % 2 == 0) {
          external.emplace_back(program);
        } else {
          internal.push_back(pool.submit(program));
        }
      }
      for (auto& t : external) t.join();
      for (auto& h : internal) h.get();
      totals.Add(pool.stats());
    }
    for (int o = 0; o < kOwners; ++o) {
      const auto k = static_cast<std::size_t>(o);
      ExpectVisits(visits[k], expected[k]);
      tasks += expected[k].count;
      throwing_rounds += threw[k];
    }
    if (HasFailure()) return;
  }
  totals.Print("reused task groups", std::to_string(tasks) + " group tasks, " + std::to_string(throwing_rounds) +
                                         " rounds rethrew an exception");
}

// ---------------------------------------------------------------------------------------
// Uneven work: parallel_for over iterations whose cost varies by orders of magnitude
// (and is clustered, so a static split would be badly unbalanced), including nested
// loops. Every index must be visited exactly once.

TEST(PoolStressTest, UnevenParallelForVisitsEveryIndexOnce) {
  stress::PrintHeader("PoolStressTest.UnevenParallelForVisitsEveryIndexOnce");
  const std::size_t n = stress::Scaled(40000, 64);
  PoolTotals totals;
  std::uint64_t iterations = 0;
  std::uint64_t index = 0;
  for (std::size_t workers : {1u, 2u, 5u, 12u}) {
    const std::uint64_t seed = stress::DerivedSeed(30, index++);
    SCOPED_TRACE(stress::ReproText(seed, "workers " + std::to_string(workers)));
    ThreadPool pool(workers);
    std::vector<std::atomic<std::uint32_t>> hits(n);
    std::atomic<std::uint64_t> inner{0};
    Rng rng(seed);
    const std::size_t heavy_begin = static_cast<std::size_t>(rng.below(n));
    const std::size_t heavy_end = std::min(n, heavy_begin + n / 16 + 1);
    for (std::size_t grain : {std::size_t{0}, std::size_t{1}, std::size_t{7}, n / 3 + 1}) {
      for (auto& h : hits) h.store(0, std::memory_order_relaxed);
      stealpool::parallel_for(pool, 0, n, grain, [&](std::size_t i) {
        const std::uint64_t s = Mix(seed ^ i);
        const bool heavy = (i >= heavy_begin && i < heavy_end) || s % 997 == 0;
        BusyWork(s);
        if (heavy) {
          volatile std::uint64_t sink = s;
          for (int k = 0; k < 2000; ++k) sink = sink * 33 + static_cast<std::uint64_t>(k);
        }
        if (s % 211 == 0) {
          // A nested loop inside one iteration: its tasks join the same deques.
          stealpool::parallel_for(pool, 0, 64, 4, [&inner](std::size_t) { inner.fetch_add(1); });
        }
        hits[i].fetch_add(1, std::memory_order_relaxed);
      });
      std::size_t wrong = 0;
      for (std::size_t i = 0; i < n; ++i) {
        if (hits[i].load(std::memory_order_relaxed) != 1 && wrong++ < 5) {
          ADD_FAILURE() << "index " << i << " visited " << hits[i].load() << " times (grain " << grain << ")";
        }
      }
      EXPECT_EQ(wrong, 0u) << "grain " << grain;
      iterations += n;
    }
    std::uint64_t expected_inner = 0;
    for (std::size_t i = 0; i < n; ++i) expected_inner += (Mix(seed ^ i) % 211 == 0) ? 64 : 0;
    EXPECT_EQ(inner.load(), 4 * expected_inner);
    iterations += inner.load();
    totals.Add(pool.stats());
    if (HasFailure()) return;
  }
  totals.Print("uneven parallel_for", std::to_string(iterations) + " loop iterations");
}

// ---------------------------------------------------------------------------------------
// External submitters doing a random mix of everything at once: submit + get, detached
// submits, blocking TaskGroup waits, parallel_for from outside the pool, and handles
// handed to pool tasks that wait on them (helping wait on a handle produced elsewhere).

TEST(PoolStressTest, ConcurrentExternalSubmittersMixedOperations) {
  stress::PrintHeader("PoolStressTest.ConcurrentExternalSubmittersMixedOperations");
  const std::uint64_t ops_per_submitter = stress::Scaled(3000, 10);
  PoolTotals totals;
  std::uint64_t operations = 0;
  std::uint64_t index = 0;
  for (std::size_t workers : {1u, 4u, 9u}) {
    for (int submitters : {2, 6}) {
      const std::uint64_t seed = stress::DerivedSeed(40, index++);
      SCOPED_TRACE(stress::ReproText(
          seed, "workers " + std::to_string(workers) + ", submitters " + std::to_string(submitters)));
      std::atomic<std::uint64_t> sum{0};          // every unit of work adds its value here
      std::vector<std::uint64_t> expected_sum(static_cast<std::size_t>(submitters), 0);
      {
        ThreadPool pool(workers);
        std::vector<std::thread> threads;
        for (int s = 0; s < submitters; ++s) {
          threads.emplace_back([&, s] {
            Rng rng(seed + static_cast<std::uint64_t>(s) * 7919);
            std::uint64_t& want = expected_sum[static_cast<std::size_t>(s)];
            for (std::uint64_t op = 0; op < ops_per_submitter; ++op) {
              const std::uint64_t value = 1 + rng.below(1000);
              switch (rng.below(5)) {
                case 0: {  // submit and wait
                  auto h = pool.submit([&sum, value] {
                    BusyWork(value);
                    sum.fetch_add(value, std::memory_order_relaxed);
                    return value;
                  });
                  EXPECT_EQ(h.get(), value);
                  want += value;
                  break;
                }
                case 1:  // detached: the pool's destructor drains it
                  (void)pool.submit([&sum, value] { sum.fetch_add(value, std::memory_order_relaxed); });
                  want += value;
                  break;
                case 2: {  // blocking group wait from outside the pool
                  TaskGroup group(pool);
                  const std::uint64_t k = 1 + rng.below(8);
                  for (std::uint64_t i = 0; i < k; ++i) {
                    group.spawn([&sum, value] {
                      BusyWork(value);
                      sum.fetch_add(value, std::memory_order_relaxed);
                    });
                  }
                  group.wait();
                  want += k * value;
                  break;
                }
                case 3: {  // parallel_for from outside the pool
                  const std::size_t len = 1 + static_cast<std::size_t>(rng.below(64));
                  stealpool::parallel_for(pool, 0, len, 1,
                                          [&sum, value](std::size_t) { sum.fetch_add(value, std::memory_order_relaxed); });
                  want += len * value;
                  break;
                }
                default: {  // a pool task waits on a handle produced by another pool task
                  auto producer = pool.submit([&sum, value] {
                    BusyWork(value);
                    sum.fetch_add(value, std::memory_order_relaxed);
                    return value * 2;
                  });
                  auto consumer = pool.submit([p = std::move(producer)]() mutable { return p.get() + 1; });
                  EXPECT_EQ(consumer.get(), value * 2 + 1);
                  want += value;
                  break;
                }
              }
            }
          });
        }
        for (auto& t : threads) t.join();
        totals.Add(pool.stats());
      }  // destructor drains the detached tasks
      std::uint64_t want = 0;
      for (std::uint64_t w : expected_sum) want += w;
      EXPECT_EQ(sum.load(), want);
      operations += ops_per_submitter * static_cast<std::uint64_t>(submitters);
      if (HasFailure()) return;
    }
  }
  totals.Print("external submitters", std::to_string(operations) + " submitter operations");
}

// ---------------------------------------------------------------------------------------
// Shutdown racing with work: submitters fire detached task trees whose nodes keep
// submitting detached children (and some nested groups) from the workers, and the pool is
// destroyed the moment the submitters return, while the trees are still growing. The
// drain must run every node; handles kept by the caller must be usable after the pool is
// gone. Some detached tasks throw; their exceptions die with their handles.

void FireTree(ThreadPool& pool, Visits& v, std::uint64_t seed, int depth) {
  v.Record(seed);
  BusyWork(seed);
  const int fanout = TreeFanout(seed, depth);
  if (fanout != 0 && TreeMode(seed) == ForkMode::kGroup) {
    TaskGroup group(pool);  // a helping wait while the destructor is already draining
    for (int i = 0; i < fanout; ++i) {
      const std::uint64_t child = TreeChild(seed, i);
      group.spawn([&pool, &v, child, depth] { FireTree(pool, v, child, depth - 1); });
    }
    group.wait();
  } else {
    for (int i = 0; i < fanout; ++i) {
      const std::uint64_t child = TreeChild(seed, i);
      (void)pool.submit([&pool, &v, child, depth] { FireTree(pool, v, child, depth - 1); });
    }
  }
  if (Mix(seed ^ 0xfeedULL) % 50 == 0) throw StressError(seed);
}

TEST(PoolStressTest, ShutdownRacesWithGrowingWork) {
  stress::PrintHeader("PoolStressTest.ShutdownRacesWithGrowingWork");
  const std::uint64_t iterations = stress::Scaled(200, 3);
  constexpr int kDepth = 7;
  std::uint64_t nodes = 0;
  for (std::uint64_t it = 0; it < iterations; ++it) {
    const std::uint64_t seed = stress::DerivedSeed(50, it);
    Rng rng(seed);
    const std::size_t workers = 1 + static_cast<std::size_t>(rng.below(6));
    const int submitters = 1 + static_cast<int>(rng.below(3));
    SCOPED_TRACE(stress::ReproText(
        seed, "workers " + std::to_string(workers) + ", submitters " + std::to_string(submitters)));
    Visits visits;
    Expected expected;
    std::vector<std::uint64_t> roots;
    for (int s = 0; s < submitters; ++s) {
      roots.push_back(Mix(seed + static_cast<std::uint64_t>(s) + 1));
      ExpectTree(expected, roots.back(), kDepth);
    }
    std::vector<TaskHandle<std::uint64_t>> kept(static_cast<std::size_t>(submitters));
    std::optional<ThreadPool> pool;
    pool.emplace(workers);
    std::vector<std::thread> threads;
    for (int s = 0; s < submitters; ++s) {
      threads.emplace_back([&, s] {
        const std::uint64_t root = roots[static_cast<std::size_t>(s)];
        ThreadPool& p = *pool;
        (void)p.submit([&p, &visits, root] { FireTree(p, visits, root, kDepth); });
        kept[static_cast<std::size_t>(s)] = p.submit([root] { return root; });
      });
    }
    for (auto& t : threads) t.join();
    // No synchronisation with the trees on purpose: they are still spawning.
    pool.reset();
    ExpectVisits(visits, expected);
    for (int s = 0; s < submitters; ++s) {
      auto& h = kept[static_cast<std::size_t>(s)];
      EXPECT_TRUE(h.is_ready());
      EXPECT_EQ(h.get(), roots[static_cast<std::size_t>(s)]);
    }
    nodes += expected.count;
    if (HasFailure()) return;
  }
  std::printf("[stress] shutdown races: %llu pools destroyed while trees grew, %llu tree nodes drained\n",
              U(iterations), U(nodes));
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------
// Two pools: tasks of pool A fork work into pool B and block on it (a worker of one pool
// waiting on another pool is an external, blocking wait), while B's tasks fork within B
// and also feed detached work back into A.

TEST(PoolStressTest, TasksWaitingAcrossPools) {
  stress::PrintHeader("PoolStressTest.TasksWaitingAcrossPools");
  const std::uint64_t outer = stress::Scaled(1500, 4);
  PoolTotals totals;
  std::uint64_t index = 0;
  std::uint64_t units = 0;
  const std::pair<std::size_t, std::size_t> shapes[] = {{2, 2}, {4, 1}, {1, 4}};
  for (const auto& [a_workers, b_workers] : shapes) {
    const std::uint64_t seed = stress::DerivedSeed(60, index++);
    SCOPED_TRACE(stress::ReproText(
        seed, "pool A " + std::to_string(a_workers) + " workers, pool B " + std::to_string(b_workers) + " workers"));
    std::atomic<std::uint64_t> sum{0};
    std::uint64_t want = 0;
    for (std::uint64_t i = 0; i < outer; ++i) want += 3 * ((Mix(seed + i) % 100) + 1);
    {
      ThreadPool a(a_workers);
      {
        ThreadPool b(b_workers);
        stealpool::parallel_for(a, 0, outer, 1, [&](std::size_t i) {
          const std::uint64_t value = (Mix(seed + i) % 100) + 1;
          auto h = b.submit([&a, &b, &sum, value] {
            TaskGroup inner(b);
            inner.spawn([&sum, value] { sum.fetch_add(value, std::memory_order_relaxed); });
            (void)a.submit([&sum, value] { sum.fetch_add(value, std::memory_order_relaxed); });
            inner.wait();
            return value;
          });
          sum.fetch_add(h.get(), std::memory_order_relaxed);
        });
        totals.Add(b.stats());
      }
      // b is gone, so nothing feeds `a` any more; destroying `a` drains the detached tasks.
      totals.Add(a.stats());
    }
    EXPECT_EQ(sum.load(), want);
    units += outer;
    if (HasFailure()) return;
  }
  totals.Print("cross-pool waits", std::to_string(units) + " cross-pool round trips");
}

}  // namespace
