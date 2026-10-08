// Baseline thread pool with a single shared task queue.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "stealpool/deque.hpp"  // kCacheLineSize
#include "stealpool/detail/task.hpp"
#include "stealpool/executor.hpp"

namespace stealpool {

// The comparison point for ThreadPool: every task, whoever schedules it, goes through one
// mutex-protected double-ended queue that all workers pop from. A worker takes its own
// newest entry if it has one queued and the oldest entry otherwise, which is ThreadPool's
// order (own deque LIFO, steals FIFO) with all per-worker deques merged into one; see
// shared_queue_pool.cpp for why helping waits need exactly this rule.
//
// Everything except the queue is deliberately the same as in ThreadPool, so a benchmark
// that swaps one pool for the other measures the queue design and nothing else:
//   * the same task objects (submit(), TaskGroup and the parallel algorithms work unchanged);
//   * the same helping wait: a worker that waits for an event keeps running queued tasks
//     and parks on its own Parker only when there is nothing to run;
//   * the same idle policy: 32 rounds of yield-and-retry, then a targeted wakeup through a
//     per-worker Parker (a wake pops one parked worker, never a broadcast);
//   * the same drain-on-destruction semantics.
//
// The per-worker Parkers play the role of the queue's condition variable. A single
// std::condition_variable cannot serve a helping waiter, which has to wake up either for
// new work or for the completion of the event it waits on (CompletionEvent signals the
// waiter's Parker directly).
//
// Shutdown: the destructor drains. Every task scheduled before the destructor was
// entered, and every task those tasks spawn, runs before the workers are joined.
// Scheduling from a non-worker thread once the destructor has started, or destroying the
// pool from one of its own workers, is undefined behaviour.
class SharedQueuePool final : public Executor {
 public:
  struct Stats {
    std::uint64_t tasks_executed = 0;  // tasks run by workers (helping waits included)
    std::uint64_t parks = 0;           // times a worker went to sleep
  };

  // Throws std::invalid_argument if num_threads is zero.
  explicit SharedQueuePool(std::size_t num_threads = default_concurrency());
  ~SharedQueuePool() override;

  std::size_t concurrency() const noexcept override { return workers_.size(); }
  void schedule(detail::Task* task) override;
  void wait_for(detail::CompletionEvent& event) override;

  // Index of the calling thread among this pool's workers, or nullopt for any other thread.
  std::optional<std::size_t> current_worker_index() const noexcept;

  // Counters summed over all workers. Approximate while tasks are running.
  Stats stats() const noexcept;

  // std::thread::hardware_concurrency(), or 1 if that is unknown.
  static std::size_t default_concurrency() noexcept;

 private:
  struct Worker;

  Worker* this_worker() const noexcept;
  void worker_main(Worker& self);
  void stop_and_join() noexcept;
  // Queue entry: the task and the index of the worker that pushed it (kExternal for any
  // other thread), so a worker can find its own most recent child.
  struct Entry {
    detail::Task* task;
    std::size_t pusher;
  };
  static constexpr std::size_t kExternal = static_cast<std::size_t>(-1);

  detail::Task* try_pop(Worker& self);
  detail::Task* pop_locked(Worker& self);
  void run_task(Worker& self, detail::Task* task) noexcept;
  template <class Done>
  detail::Task* park(Worker& self, bool main_loop, Done&& done);
  void wake_all_locked(std::vector<Worker*>& to_unpark);

  std::vector<std::unique_ptr<Worker>> workers_;

  // The queue and all sleep state share one mutex: this pool is the "one global lock"
  // design. `size_` mirrors queue_.size() so idle workers can poll without taking the lock
  // (ThreadPool's injection queue does the same); correctness never depends on it, since
  // every decision to sleep is re-checked under the mutex.
  alignas(kCacheLineSize) std::mutex mutex_;
  std::deque<Entry> queue_;                  // guarded by mutex_
  std::vector<Worker*> idle_;                // parked workers; guarded
  std::size_t main_idle_ = 0;                // workers parked in the main loop; guarded
  bool stopping_ = false;                    // destructor entered; guarded
  std::atomic<bool> exiting_{false};         // quiescent after stop: workers leave
  alignas(kCacheLineSize) std::atomic<std::size_t> size_{0};
};

}  // namespace stealpool
