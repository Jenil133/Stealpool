// Randomized stress tests for WorkStealingDeque (label "stress").
//
// Every element is a pointer to an Item whose plain `payload` field the owner writes
// just before pushing it. Whoever takes the element reads the payload without any
// extra synchronisation, so a missing happens-before edge in the deque shows up as a
// ThreadSanitizer report or as a wrong payload, and the per-item `taken` counter shows
// loss (0) or duplication (> 1). Seeds and operation totals are printed; see
// stress_util.hpp for STEALPOOL_STRESS_SEED / STEALPOOL_STRESS_SCALE.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "stealpool/deque.hpp"
#include "stress_util.hpp"

namespace {

using stealpool::WorkStealingDeque;
using stress::Rng;
using stress::U;

struct Item {
  std::uint64_t payload = 0;  // written by the producer before the push, plain on purpose
  std::atomic<std::uint32_t> taken{0};
};

using Deque = WorkStealingDeque<Item*>;
using StealStatus = Deque::StealStatus;

// Owns the items of one run and checks every take.
class Ledger {
 public:
  Ledger(std::size_t count, std::uint64_t seed) : items_(count), seed_(seed) {}

  std::size_t size() const { return items_.size(); }

  // Producer side: fill in the payload, then hand out the pointer for pushing.
  Item* Prepare(std::size_t index) {
    Item& item = items_[index];
    item.payload = Expected(index);
    return &item;
  }

  // Consumer side; returns the index of the taken item (or SIZE_MAX if the pointer was
  // garbage, which is counted as a failure).
  std::size_t Take(Item* item) {
    if (item < items_.data() || item >= items_.data() + items_.size()) {
      bad_pointers_.fetch_add(1, std::memory_order_relaxed);
      return SIZE_MAX;
    }
    const auto index = static_cast<std::size_t>(item - items_.data());
    if (item->payload != Expected(index)) bad_payloads_.fetch_add(1, std::memory_order_relaxed);
    item->taken.fetch_add(1, std::memory_order_relaxed);
    taken_total_.fetch_add(1, std::memory_order_relaxed);
    return index;
  }

  std::uint64_t taken_total() const { return taken_total_.load(std::memory_order_relaxed); }

  // Call after every thread joined. Reports the first few problems and returns false on
  // any loss, duplication or corruption.
  bool Verify(std::size_t produced) const {
    bool ok = true;
    int reported = 0;
    for (std::size_t i = 0; i < items_.size(); ++i) {
      const std::uint32_t expected = i < produced ? 1 : 0;
      const std::uint32_t got = items_[i].taken.load(std::memory_order_relaxed);
      if (got != expected) {
        ok = false;
        if (reported++ < 5) ADD_FAILURE() << "item " << i << " taken " << got << " times, expected " << expected;
      }
    }
    if (bad_pointers_.load() != 0) {
      ok = false;
      ADD_FAILURE() << bad_pointers_.load() << " taken pointers did not point at an item";
    }
    if (bad_payloads_.load() != 0) {
      ok = false;
      ADD_FAILURE() << bad_payloads_.load() << " taken items had a wrong payload";
    }
    return ok;
  }

 private:
  std::uint64_t Expected(std::size_t index) const { return stress::Mix(seed_ ^ (index * 0x9e3779b97f4a7c15ULL)); }

  std::vector<Item> items_;
  std::uint64_t seed_;
  std::atomic<std::uint64_t> bad_pointers_{0};
  std::atomic<std::uint64_t> bad_payloads_{0};
  std::atomic<std::uint64_t> taken_total_{0};
};

struct Totals {
  std::uint64_t runs = 0;
  std::uint64_t pushes = 0;
  std::uint64_t pop_attempts = 0;
  std::uint64_t pops = 0;  // successful
  std::uint64_t steal_attempts = 0;
  std::uint64_t steals = 0;  // successful
  std::uint64_t steal_empty = 0;
  std::uint64_t steal_lost = 0;
  std::uint64_t max_capacity = 0;

  void Add(const Totals& o) {
    runs += o.runs;
    pushes += o.pushes;
    pop_attempts += o.pop_attempts;
    pops += o.pops;
    steal_attempts += o.steal_attempts;
    steals += o.steals;
    steal_empty += o.steal_empty;
    steal_lost += o.steal_lost;
    max_capacity = std::max(max_capacity, o.max_capacity);
  }

  void Print(const char* what) const {
    const std::uint64_t ops = pushes + pop_attempts + steal_attempts;
    std::printf(
        "[stress] %s: %llu runs, %llu pushes, %llu pops (%llu attempts), %llu steals (%llu attempts: %llu empty, "
        "%llu lost race), largest ring %llu, total deque operations %llu\n",
        what, U(runs), U(pushes), U(pops), U(pop_attempts), U(steals), U(steal_attempts), U(steal_empty),
        U(steal_lost), U(max_capacity), U(ops));
    std::fflush(stdout);
  }
};

// Thief-side counters, one per thread, merged after join.
struct ThiefCounts {
  std::uint64_t attempts = 0;
  std::uint64_t success = 0;
  std::uint64_t empty = 0;
  std::uint64_t lost = 0;
  std::uint64_t order_violations = 0;
};

// One steal attempt with bookkeeping. Returns the status.
StealStatus StealOnce(Deque& dq, Ledger& ledger, ThiefCounts& counts, std::size_t& last_index) {
  Item* item = nullptr;
  const StealStatus status = dq.try_steal(item);
  ++counts.attempts;
  switch (status) {
    case StealStatus::kSuccess: {
      ++counts.success;
      const std::size_t index = ledger.Take(item);
      // Items are pushed in increasing index order, and top_ only moves forward, so the
      // items one thief steals from one deque have strictly increasing indices (a slot
      // that is popped and refilled is refilled with a newer item).
      if (index != SIZE_MAX) {
        if (last_index != SIZE_MAX && index <= last_index) ++counts.order_violations;
        last_index = index;
      }
      break;
    }
    case StealStatus::kEmpty:
      ++counts.empty;
      break;
    case StealStatus::kLostRace:
      ++counts.lost;
      break;
  }
  return status;
}

// Starts `thieves` threads that steal from `dq` until `owner_done` is set and the deque
// is empty. Thieves occasionally yield so interleavings vary between attempts.
class ThiefCrew {
 public:
  ThiefCrew(Deque& dq, Ledger& ledger, int thieves, std::uint64_t seed) : counts_(static_cast<std::size_t>(thieves)) {
    for (int t = 0; t < thieves; ++t) {
      threads_.emplace_back([this, &dq, &ledger, t, seed] {
        ThiefCounts& counts = counts_[static_cast<std::size_t>(t)];
        Rng rng(seed + static_cast<std::uint64_t>(t) + 1);
        std::size_t last_index = SIZE_MAX;
        for (;;) {
          // acquire: once the owner is done, its final bottom_ is visible, so kEmpty
          // after this point really means empty for good.
          const bool done = owner_done_.load(std::memory_order_acquire);
          const StealStatus status = StealOnce(dq, ledger, counts, last_index);
          if (done && status == StealStatus::kEmpty) break;
          if (status != StealStatus::kSuccess && rng.below(64) == 0) std::this_thread::yield();
        }
      });
    }
  }

  // Owner calls this after its last operation on the deque.
  ThiefCounts Finish(Totals& totals) {
    owner_done_.store(true, std::memory_order_release);
    for (auto& t : threads_) t.join();
    ThiefCounts sum;
    for (const auto& c : counts_) {
      sum.attempts += c.attempts;
      sum.success += c.success;
      sum.empty += c.empty;
      sum.lost += c.lost;
      sum.order_violations += c.order_violations;
    }
    totals.steal_attempts += sum.attempts;
    totals.steals += sum.success;
    totals.steal_empty += sum.empty;
    totals.steal_lost += sum.lost;
    return sum;
  }

  ThiefCrew(const ThiefCrew&) = delete;
  ThiefCrew& operator=(const ThiefCrew&) = delete;

 private:
  std::vector<ThiefCounts> counts_;
  std::vector<std::thread> threads_;
  std::atomic<bool> owner_done_{false};
};

// Owner-side helpers that keep the counters.
struct Owner {
  Deque& dq;
  Ledger& ledger;
  Totals& totals;
  std::size_t next = 0;

  bool Push() {
    if (next >= ledger.size()) return false;
    dq.push(ledger.Prepare(next++));
    ++totals.pushes;
    return true;
  }
  bool Pop() {
    ++totals.pop_attempts;
    if (auto item = dq.pop()) {
      ++totals.pops;
      ledger.Take(*item);
      return true;
    }
    return false;
  }
  void Drain() {
    while (Pop()) {
    }
  }
  void NoteCapacity() { totals.max_capacity = std::max<std::uint64_t>(totals.max_capacity, dq.capacity()); }
};

// The owner runs a random program of phases until every item was pushed:
//   burst push   (1..4096 pushes: repeated ring growth while thieves read old rings)
//   burst pop    (pops racing thieves from the other end)
//   mixed        (per operation coin flip between push and pop, 1:1 to 4:1)
//   trickle      (drain, then push one / pop one: the last-element race every time)
// then drains the deque. Every item must be taken exactly once.
Totals RunRandomProgram(std::uint64_t seed, int thieves, std::size_t items) {
  Totals totals;
  totals.runs = 1;
  Deque dq(2);  // tiny initial ring: growth happens many times per run
  Ledger ledger(items, seed);
  ThiefCrew crew(dq, ledger, thieves, seed);
  Owner owner{dq, ledger, totals};
  Rng rng(seed);

  while (owner.next < items) {
    switch (rng.below(5)) {
      case 0:
      case 1: {
        const std::uint64_t burst = 1 + rng.below(rng.chance(10) ? 4096 : 256);
        for (std::uint64_t i = 0; i < burst && owner.Push(); ++i) {
        }
        owner.NoteCapacity();
        break;
      }
      case 2: {
        const std::uint64_t burst = 1 + rng.below(256);
        for (std::uint64_t i = 0; i < burst; ++i) owner.Pop();
        break;
      }
      case 3: {
        const std::uint64_t ops = 1 + rng.below(512);
        const unsigned push_percent = 50 + static_cast<unsigned>(rng.below(31));
        for (std::uint64_t i = 0; i < ops; ++i) {
          if (rng.chance(push_percent)) {
            owner.Push();
          } else {
            owner.Pop();
          }
        }
        break;
      }
      default: {
        owner.Drain();
        const std::uint64_t rounds = 1 + rng.below(128);
        for (std::uint64_t i = 0; i < rounds && owner.Push(); ++i) owner.Pop();
        break;
      }
    }
  }
  owner.Drain();
  owner.NoteCapacity();
  const ThiefCounts thief = crew.Finish(totals);

  EXPECT_TRUE(ledger.Verify(items));
  EXPECT_EQ(totals.pops + thief.success, items) << "pops + steals must equal pushes";
  EXPECT_EQ(thief.order_violations, 0u) << "a thief stole items out of push order";
  return totals;
}

TEST(DequeStressTest, OwnerRandomProgramWithThieves) {
  stress::PrintHeader("DequeStressTest.OwnerRandomProgramWithThieves");
  const std::size_t items = stress::Scaled(250000, 100);
  Totals totals;
  std::uint64_t index = 0;
  for (int round = 0; round < 3; ++round) {
    for (int thieves : {1, 2, 4, 7}) {
      const std::uint64_t seed = stress::DerivedSeed(1, index++);
      SCOPED_TRACE(stress::ReproText(seed, "thieves " + std::to_string(thieves) + ", items " + std::to_string(items)));
      totals.Add(RunRandomProgram(seed, thieves, items));
      if (HasFailure()) {
        totals.Print("deque random program (stopped at first failure)");
        return;
      }
    }
  }
  totals.Print("deque random program");
}

// Owner pushes 1..3 items and pops until empty, over and over, while thieves hammer the
// same few slots: the owner's pop of the last element and a thief's steal resolve
// through the same CAS on top_, and exactly one of them may win.
TEST(DequeStressTest, LastElementRaces) {
  stress::PrintHeader("DequeStressTest.LastElementRaces");
  const std::size_t items = stress::Scaled(150000, 100);
  Totals totals;
  std::uint64_t index = 0;
  for (int thieves : {1, 3, 6}) {
    const std::uint64_t seed = stress::DerivedSeed(2, index++);
    SCOPED_TRACE(stress::ReproText(seed, "thieves " + std::to_string(thieves)));
    Totals run;
    run.runs = 1;
    Deque dq(2);
    Ledger ledger(items, seed);
    ThiefCrew crew(dq, ledger, thieves, seed);
    Owner owner{dq, ledger, run};
    Rng rng(seed);
    while (owner.next < items) {
      const std::uint64_t n = 1 + rng.below(3);
      for (std::uint64_t i = 0; i < n; ++i) owner.Push();
      owner.Drain();
    }
    owner.NoteCapacity();
    const ThiefCounts thief = crew.Finish(run);
    EXPECT_TRUE(ledger.Verify(items));
    EXPECT_EQ(run.pops + thief.success, items);
    EXPECT_EQ(thief.order_violations, 0u);
    totals.Add(run);
  }
  totals.Print("deque last-element races");
}

// Forced growth: the deque starts at capacity 2 and is pre-filled before the thieves
// start (so it has grown at least to the pre-fill size no matter how the threads are
// scheduled), then the owner keeps pushing large bursts while thieves steal, so further
// resizes copy rings that thieves are reading at that moment. Repeated with fresh deques.
TEST(DequeStressTest, GrowthWhileThievesSteal) {
  stress::PrintHeader("DequeStressTest.GrowthWhileThievesSteal");
  const std::size_t items = stress::Scaled(200000, 2000);
  constexpr std::size_t kPrefill = 1000;
  Totals totals;
  std::uint64_t index = 0;
  for (int round = 0; round < 2; ++round) {
    for (int thieves : {2, 5}) {
      const std::uint64_t seed = stress::DerivedSeed(3, index++);
      SCOPED_TRACE(stress::ReproText(seed, "thieves " + std::to_string(thieves)));
      Totals run;
      run.runs = 1;
      Deque dq(2);
      Ledger ledger(items, seed);
      Owner owner{dq, ledger, run};
      for (std::size_t i = 0; i < kPrefill; ++i) owner.Push();
      EXPECT_GE(dq.capacity(), kPrefill);
      ThiefCrew crew(dq, ledger, thieves, seed);
      Rng rng(seed);
      while (owner.next < items) {
        // Large push bursts, small pop bursts: the size keeps climbing until the end.
        const std::uint64_t burst = 1000 + rng.below(16000);
        for (std::uint64_t i = 0; i < burst && owner.Push(); ++i) {
        }
        owner.NoteCapacity();
        for (std::uint64_t i = rng.below(64); i > 0; --i) owner.Pop();
      }
      owner.Drain();
      const ThiefCounts thief = crew.Finish(run);
      EXPECT_TRUE(ledger.Verify(items));
      EXPECT_EQ(run.pops + thief.success, items);
      EXPECT_EQ(thief.order_violations, 0u);
      totals.Add(run);
    }
  }
  totals.Print("deque growth under stealing");
}

// The scheduler's pattern: every thread owns one deque, pushes and pops its own items and
// steals from random other deques whenever its own is empty. Runs until every item of
// every owner was taken exactly once.
TEST(DequeStressTest, EveryThreadOwnerAndThief) {
  stress::PrintHeader("DequeStressTest.EveryThreadOwnerAndThief");
  const std::size_t per_owner = stress::Scaled(100000, 100);
  Totals totals;
  std::uint64_t index = 0;
  for (int threads : {2, 4, 8}) {
    const std::uint64_t seed = stress::DerivedSeed(4, index++);
    SCOPED_TRACE(stress::ReproText(seed, "threads " + std::to_string(threads)));
    const auto n = static_cast<std::size_t>(threads);
    std::vector<std::unique_ptr<Deque>> deques;
    std::vector<std::unique_ptr<Ledger>> ledgers;
    for (std::size_t i = 0; i < n; ++i) {
      deques.push_back(std::make_unique<Deque>(2));
      ledgers.push_back(std::make_unique<Ledger>(per_owner, seed + i));
    }
    const std::uint64_t total_items = per_owner * n;
    std::atomic<std::uint64_t> taken{0};
    std::vector<Totals> per_thread(n);
    std::vector<ThiefCounts> thief_counts(n);

    std::vector<std::thread> workers;
    for (std::size_t w = 0; w < n; ++w) {
      workers.emplace_back([&, w] {
        Totals& t = per_thread[w];
        ThiefCounts& tc = thief_counts[w];
        Owner owner{*deques[w], *ledgers[w], t};
        Rng rng(seed ^ stress::Mix(w + 100));
        // Odd threads are mostly producers that rarely pop their own deque, so the even
        // threads run dry and have to steal from them (otherwise owners take nearly
        // everything themselves and the steal path is barely exercised).
        const unsigned pop_percent = w % 2 == 1 ? 10 : 90;
        // Last stolen index per victim (order check is per deque).
        std::vector<std::size_t> last(n, SIZE_MAX);
        while (taken.load(std::memory_order_relaxed) < total_items) {
          std::uint64_t got = 0;
          if (owner.next < per_owner && rng.chance(60)) {
            for (std::uint64_t i = 1 + rng.below(64); i > 0 && owner.Push(); --i) {
            }
            owner.NoteCapacity();
          }
          if (rng.chance(pop_percent)) {
            for (std::uint64_t i = rng.below(48); i > 0; --i) {
              if (!owner.Pop()) break;
              ++got;
            }
          }
          if (owner.dq.empty_approx()) {
            // Own deque looks empty: steal like an idle worker, one victim per attempt.
            const std::size_t victim = static_cast<std::size_t>(rng.below(n));
            if (victim != w &&
                StealOnce(*deques[victim], *ledgers[victim], tc, last[victim]) == StealStatus::kSuccess) {
              ++got;
            }
          }
          if (got != 0) {
            taken.fetch_add(got, std::memory_order_relaxed);
          } else {
            std::this_thread::yield();
          }
        }
      });
    }
    for (auto& t : workers) t.join();

    Totals run;
    run.runs = 1;
    std::uint64_t order_violations = 0;
    for (std::size_t w = 0; w < n; ++w) {
      run.Add(per_thread[w]);
      run.steal_attempts += thief_counts[w].attempts;
      run.steals += thief_counts[w].success;
      run.steal_empty += thief_counts[w].empty;
      run.steal_lost += thief_counts[w].lost;
      order_violations += thief_counts[w].order_violations;
      EXPECT_TRUE(ledgers[w]->Verify(per_owner)) << "owner " << w;
    }
    EXPECT_EQ(run.pushes, total_items);
    EXPECT_EQ(run.pops + run.steals, total_items);
    EXPECT_EQ(order_violations, 0u);
    totals.Add(run);
  }
  totals.Print("deque mesh (every thread owner and thief)");
}

}  // namespace
