// Low-level building blocks shared by every executor: the intrusive task object,
// the per-thread parker and the single-waiter completion event.
#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace stealpool::detail {

// A unit of work as seen by the scheduler: an intrusive header placed at the start of
// a heap object that also holds the callable (and, for submit(), the result). One
// allocation per task, no std::function, and the queues move a single pointer.
//
// The scheduler calls execute() exactly once. The concrete type behind `run_` runs the
// callable, reports completion and frees whatever it owns, so after execute() returns
// the pointer must not be touched again.
class Task {
 public:
  using RunFn = void (*)(Task*) noexcept;

  void execute() noexcept { run_(this); }

 protected:
  explicit Task(RunFn run) noexcept : run_(run) {}
  ~Task() = default;  // destroyed only through the concrete type's RunFn

 private:
  RunFn run_;
};

// Binary semaphore used to block one thread until another thread signals it. A signal
// sent before the owner parks is remembered (the flag), so it can never be lost.
//
// unpark() notifies while still holding the mutex: a parked external thread may own the
// Parker on its stack, and it can only observe `notified_` (and then return and destroy
// the Parker) after the signalling thread has released the lock, i.e. after notify_one()
// has returned.
class Parker {
 public:
  void park() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return notified_; });
    notified_ = false;
  }

  void unpark() {
    std::lock_guard lock(mutex_);
    notified_ = true;
    cv_.notify_one();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool notified_ = false;
};

// One-shot, single-waiter completion flag.
//
// State word: 0 = not set and nobody waiting, 1 = set, anything else = the Parker of
// the (single) thread waiting for it. Both transitions are one atomic RMW, which gives
// two properties the waiting protocol depends on:
//   * no lost wakeup: either the waiter's CAS installs its Parker before set()'s
//     exchange (and set() returns that Parker so the caller unparks it), or the CAS
//     fails because the event is already set and the waiter never parks;
//   * set()'s exchange is the setter's last access to the event, so the waiter may
//     destroy the object holding the event as soon as it observes kSet.
class CompletionEvent {
 public:
  bool is_set() const noexcept {
    // acquire: pairs with the release half of set(); everything the setter wrote before
    // set() (results, exceptions, child task side effects) is visible to the waiter.
    return state_.load(std::memory_order_acquire) == kSet;
  }

  // Returns false if the event is already set (the caller must not park). Registering
  // while another waiter is registered breaks the single-waiter rule; a debug build
  // asserts instead of letting the second waiter return early and read an unset result.
  bool try_register(Parker* waiter) noexcept {
    std::uintptr_t expected = kEmpty;
    // acq_rel on success publishes the Parker to set(); acquire on failure has the same
    // purpose as in is_set().
    const bool registered = state_.compare_exchange_strong(
        expected, reinterpret_cast<std::uintptr_t>(waiter), std::memory_order_acq_rel, std::memory_order_acquire);
    assert((registered || expected == kSet) && "a CompletionEvent supports a single waiter");
    return registered;
  }

  // Marks the event set and returns the registered waiter, if any, which the caller must
  // unpark. Must be called at most once between resets.
  [[nodiscard]] Parker* set() noexcept {
    const std::uintptr_t previous = state_.exchange(kSet, std::memory_order_acq_rel);
    return previous > kSet ? reinterpret_cast<Parker*>(previous) : nullptr;
  }

  // Only when no setter or waiter can be active (e.g. after a completed wait).
  void reset() noexcept { state_.store(kEmpty, std::memory_order_relaxed); }

 private:
  static constexpr std::uintptr_t kEmpty = 0;
  static constexpr std::uintptr_t kSet = 1;
  std::atomic<std::uintptr_t> state_{kEmpty};
};

}  // namespace stealpool::detail
