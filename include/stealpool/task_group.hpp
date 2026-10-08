// Fork-join scope: spawn any number of tasks, then wait for all of them.
#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

#include "stealpool/detail/task.hpp"
#include "stealpool/executor.hpp"

namespace stealpool {

// Usage:
//   TaskGroup group(pool);
//   group.spawn([&] { left(); });
//   right();
//   group.wait();   // rethrows the first exception thrown by a spawned task
//
// wait() on a worker thread of `executor` runs other tasks while it waits, so groups
// nest arbitrarily deep (recursive quicksort, tree reductions) without deadlock, even
// on a single-worker pool. On any other thread it blocks. Because helping runs unrelated
// queued tasks nested on the waiter's stack, the waiting frame cannot resume until the
// nested task returns: a task must not block on another task's continuation (code that
// runs after some other wait() returns), and must not hold a lock across wait(), or the
// pool can deadlock even with idle workers.
//
// Threading rules: spawn() may be called by the group's owner or by tasks of the same
// group while they run (they keep the group open). wait() is called by the owner only.
// After wait() returns the group is empty and can be reused.
//
// Exceptions: every spawned task runs even if an earlier one threw; the first exception
// (in completion order) is kept and rethrown by wait(), later ones are dropped.
// The destructor waits for outstanding tasks and discards an unobserved exception, so
// an exception escaping the owner's scope cannot leave tasks referring to dead locals.
class TaskGroup {
 public:
  explicit TaskGroup(Executor& executor) noexcept : executor_(executor) {}
  TaskGroup(const TaskGroup&) = delete;
  TaskGroup& operator=(const TaskGroup&) = delete;

  ~TaskGroup() {
    try {
      wait();
    } catch (...) {
      // Intentionally discarded; see the class comment.
    }
  }

  template <class F>
  void spawn(F&& fn) {
    auto* task = new GroupTask<std::decay_t<F>>(std::forward<F>(fn), *this);
    // relaxed: the increment is sequenced before schedule(), which publishes the task;
    // the child's decrement therefore follows it in the counter's modification order.
    pending_.fetch_add(1, std::memory_order_relaxed);
    try {
      executor_.schedule(task);
    } catch (...) {
      pending_.fetch_sub(1, std::memory_order_relaxed);  // cannot reach zero: owner token
      GroupTask<std::decay_t<F>>::discard(task);
      throw;
    }
  }

  void wait() {
    // Fast path: only the owner token is left, i.e. every child already finished.
    // acquire: the children's decrements are releases forming one release sequence on
    // pending_, so their side effects (and error_) are visible after this load.
    if (pending_.load(std::memory_order_acquire) != 1) {
      // Drop the owner token. If a child is still running, the last child to finish
      // sets done_; until then help (worker) or block (other thread).
      if (pending_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        executor_.wait_for(done_);
      }
      // No child can touch the group any more: the last one's final access was
      // done_.set(). Re-arm for reuse.
      done_.reset();
      pending_.store(1, std::memory_order_relaxed);
    }
    if (has_error_.load(std::memory_order_relaxed)) {
      has_error_.store(false, std::memory_order_relaxed);
      std::rethrow_exception(std::exchange(error_, nullptr));
    }
  }

  Executor& executor() const noexcept { return executor_; }

 private:
  template <class F>
  class GroupTask final : public detail::Task {
   public:
    template <class G>
    GroupTask(G&& fn, TaskGroup& group) : Task(&run), fn_(std::forward<G>(fn)), group_(group) {}

    static void discard(Task* task) noexcept { delete static_cast<GroupTask*>(task); }

   private:
    static void run(Task* base) noexcept {
      auto* self = static_cast<GroupTask*>(base);
      TaskGroup& group = self->group_;
      try {
        std::invoke(self->fn_);
      } catch (...) {
        group.record_exception(std::current_exception());
      }
      // Free the task (and the callable's captures) before signalling: once the group
      // completes, the owner may destroy anything the captures refer to.
      delete self;
      group.child_done();
    }

    F fn_;
    TaskGroup& group_;
  };

  void record_exception(std::exception_ptr error) noexcept {
    // The first thread to flip the flag owns error_; the write is published to the
    // waiter by this child's release decrement in child_done().
    if (!has_error_.exchange(true, std::memory_order_relaxed)) error_ = std::move(error);
  }

  void child_done() noexcept {
    // acq_rel: release publishes this child's work; acquire (on the last decrement) makes
    // the other children's work visible before done_ is set, so the waiter sees it all.
    if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // Last access to *this. The returned Parker belongs to the waiting thread, not to
      // the group, so it is safe to use after the owner destroys the group.
      if (detail::Parker* waiter = done_.set()) waiter->unpark();
    }
  }

  Executor& executor_;
  // Number of unfinished children plus one "owner token" held until wait(). The token
  // keeps the count from touching zero while the owner is still spawning, so done_ is set
  // exactly once per wait, and only after the owner stopped spawning.
  std::atomic<std::size_t> pending_{1};
  detail::CompletionEvent done_;
  std::atomic<bool> has_error_{false};
  std::exception_ptr error_;
};

}  // namespace stealpool
