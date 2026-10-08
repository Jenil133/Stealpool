// Chase-Lev work-stealing deque.
//
// One owner thread pushes and pops at the bottom (LIFO, good cache locality for
// fork-join); any number of thief threads steal from the top (FIFO, they take the
// oldest and usually largest piece of work). The algorithm follows
//   D. Chase, Y. Lev, "Dynamic Circular Work-Stealing Deque", SPAA 2005, and
//   N. M. Le et al., "Correct and Efficient Work-Stealing for Weak Memory Models",
//   PPoPP 2013,
// with one deliberate change: the PPoPP paper orders the critical accesses with
// standalone std::atomic_thread_fence(seq_cst). ThreadSanitizer does not model
// fences, so a fence-based deque produces false reports (or hides real ones). Here
// the accesses to `top_` and `bottom_` that need store->load ordering are themselves
// seq_cst operations. On AArch64 a seq_cst store compiles to stlr, the same instruction
// as a release store; a seq_cst load compiles to ldar where an acquire load may use the
// weaker ldapr (on cores with RCpc). ldar is exactly the store->load ordering the
// protocol needs and is only slightly stronger than acquire. On x86 the seq_cst store in
// pop() becomes an xchg, which is what the fence would have cost anyway.
//
// Slots are std::atomic<T> accessed with relaxed ordering. A thief may read a slot
// that the owner is concurrently overwriting (after the ring wrapped); the thief
// then fails its CAS on `top_` and discards the value. Making the slot atomic turns
// that benign race into a well-defined one.
#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <vector>

namespace stealpool {

// Size used to keep independently written atomics on separate cache lines. The
// standard constant is avoided because it is not provided by every standard
// library and GCC warns that its value is ABI-unstable. 128 bytes is the line size of
// Apple silicon (`sysctl hw.cachelinesize` reports 128); on x86-64 the line is 64 bytes
// but the adjacent-line prefetcher pulls lines in 128-byte pairs, so 128 avoids false
// sharing on both. 64 would put two padded fields into one line on Apple silicon.
inline constexpr std::size_t kCacheLineSize = 128;

template <class T>
class WorkStealingDeque {
  static_assert(std::is_trivially_copyable_v<T>, "deque elements are copied through std::atomic<T>");
  static_assert(std::atomic<T>::is_always_lock_free, "deque elements must fit in a lock-free atomic");

 public:
  explicit WorkStealingDeque(std::size_t initial_capacity = 64) {
    std::size_t capacity = std::bit_ceil(initial_capacity < 2 ? std::size_t{2} : initial_capacity);
    auto ring = std::make_unique<Ring>(capacity);
    buffer_.store(ring.get(), std::memory_order_relaxed);
    rings_.push_back(std::move(ring));
  }

  WorkStealingDeque(const WorkStealingDeque&) = delete;
  WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

  // Owner only. Never fails; grows the ring when it is full.
  void push(T value) {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);  // only the owner writes bottom_
    // acquire: pairs with the thieves' successful CAS on top_, so once we observe that a
    // slot was stolen we may reuse it without racing with the thief's read of it.
    const std::int64_t t = top_.load(std::memory_order_acquire);
    Ring* ring = buffer_.load(std::memory_order_relaxed);  // only the owner writes buffer_
    if (b - t > static_cast<std::int64_t>(ring->capacity()) - 1) {
      ring = grow(ring, t, b);
    }
    ring->store(b, value);
    // seq_cst (at least release) publishes the slot write above to any thief that reads
    // the new bottom. It is seq_cst rather than release so that a caller can follow the
    // push with a seq_cst load of a sleeper counter: either the caller sees the sleeper or
    // the sleeper's subsequent seq_cst steal sees this element (no lost wakeup).
    bottom_.store(b + 1, std::memory_order_seq_cst);
  }

  // Owner only. Takes the most recently pushed element.
  std::optional<T> pop() {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Ring* ring = buffer_.load(std::memory_order_relaxed);
    // Reserve slot b before looking at top_. The store->load ordering between this store
    // and the top_ load below is the heart of the algorithm (a thief does the mirror image:
    // load top_, then load bottom_). Only seq_cst on both sides forbids the outcome where
    // the owner reads a stale top_ while a thief reads the old bottom_, which would let both
    // take the same element.
    bottom_.store(b, std::memory_order_seq_cst);
    std::int64_t t = top_.load(std::memory_order_seq_cst);

    if (t > b) {
      // Was already empty; undo the reservation. release keeps every bottom_ store a
      // release store, so a thief that reads any bottom_ value also sees all slot writes
      // that precede it (C++20 release sequences no longer include plain stores).
      bottom_.store(b + 1, std::memory_order_release);
      return std::nullopt;
    }

    T value = ring->load(b);
    if (t == b) {
      // Last element: race thieves for it through top_, exactly as a thief would.
      const bool won = top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                    std::memory_order_relaxed);
      bottom_.store(b + 1, std::memory_order_release);
      if (!won) return std::nullopt;
    }
    return value;
  }

  enum class StealStatus { kSuccess, kEmpty, kLostRace };

  // Any thread. Takes the oldest element. kLostRace means the deque was not empty but
  // another thread (the owner or a thief) took the element first; retrying may succeed.
  // A lost race always means some other thread made progress (lock-freedom).
  StealStatus try_steal(T& out) {
    // seq_cst load of top_ then seq_cst load of bottom_: the mirror of pop() (see above).
    // The bottom_ load also acquires the owner's slot write published by push().
    std::int64_t t = top_.load(std::memory_order_seq_cst);
    const std::int64_t b = bottom_.load(std::memory_order_seq_cst);
    if (t >= b) return StealStatus::kEmpty;

    // acquire: pairs with the release store in grow(), so the copied contents of a new
    // ring are visible. Reading an older ring is also fine: rings are never freed while
    // the deque lives, and if the element moved on, top_ moved too and the CAS fails.
    Ring* ring = buffer_.load(std::memory_order_acquire);
    T value = ring->load(t);
    // seq_cst CAS claims the element. On failure the value read above may be stale or
    // overwritten; it is discarded.
    if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
      return StealStatus::kLostRace;
    }
    out = value;
    return StealStatus::kSuccess;
  }

  // Any thread. Single attempt; nullopt if empty or the race was lost.
  std::optional<T> steal() {
    T value{};
    if (try_steal(value) == StealStatus::kSuccess) return value;
    return std::nullopt;
  }

  // Approximate (racy) size; exact when no other thread is operating on the deque.
  std::size_t size_approx() const {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    const std::int64_t t = top_.load(std::memory_order_relaxed);
    return b > t ? static_cast<std::size_t>(b - t) : 0;
  }

  bool empty_approx() const { return size_approx() == 0; }

  // Owner only (it is the only thread that resizes).
  std::size_t capacity() const { return buffer_.load(std::memory_order_relaxed)->capacity(); }

 private:
  class Ring {
   public:
    explicit Ring(std::size_t capacity)
        : mask_(capacity - 1), slots_(std::make_unique<std::atomic<T>[]>(capacity)) {
      assert(std::has_single_bit(capacity));
    }

    std::size_t capacity() const { return mask_ + 1; }

    // Relaxed: ordering is provided by the top_/bottom_/buffer_ accesses around them.
    T load(std::int64_t index) const {
      return slots_[static_cast<std::size_t>(index) & mask_].load(std::memory_order_relaxed);
    }
    void store(std::int64_t index, T value) {
      slots_[static_cast<std::size_t>(index) & mask_].store(value, std::memory_order_relaxed);
    }

   private:
    std::size_t mask_;
    std::unique_ptr<std::atomic<T>[]> slots_;
  };

  // Owner only. Copies the live range [t, b) into a ring twice as large. The old ring is
  // retired but kept alive until the deque is destroyed, because a thief that loaded the
  // old buffer_ pointer may still read from it. Retired memory is bounded by the size of
  // the final ring (capacities form a geometric series).
  Ring* grow(Ring* old, std::int64_t t, std::int64_t b) {
    auto bigger = std::make_unique<Ring>(old->capacity() * 2);
    for (std::int64_t i = t; i < b; ++i) bigger->store(i, old->load(i));
    Ring* raw = bigger.get();
    rings_.push_back(std::move(bigger));
    // release: a thief that acquires the new pointer sees the copied slots.
    buffer_.store(raw, std::memory_order_release);
    return raw;
  }

  // top_ is written by thieves, bottom_ only by the owner: separate cache lines avoid
  // false sharing between the owner's hot path and the stealing threads.
  alignas(kCacheLineSize) std::atomic<std::int64_t> top_{0};
  alignas(kCacheLineSize) std::atomic<std::int64_t> bottom_{0};
  alignas(kCacheLineSize) std::atomic<Ring*> buffer_{nullptr};
  std::vector<std::unique_ptr<Ring>> rings_;  // owner only; every ring ever allocated
};

}  // namespace stealpool
