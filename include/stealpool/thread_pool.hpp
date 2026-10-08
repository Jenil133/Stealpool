// Work-stealing thread pool.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "stealpool/deque.hpp"
#include "stealpool/detail/task.hpp"
#include "stealpool/executor.hpp"

namespace stealpool {

// N worker threads, each owning a Chase-Lev deque.
//
// * A task scheduled from one of this pool's workers goes to the bottom of that worker's
//   deque (LIFO: the most recently spawned, cache-hot task runs next).
// * A task scheduled from any other thread goes to a mutex-protected injection queue.
// * An idle worker looks for work in this order: its own deque, the other workers'
//   deques (starting at a random victim, every victim tried once), the injection queue.
//   It then retries briefly (yielding) and finally parks. Parking never loses a wakeup:
//   see the protocol description in thread_pool.cpp.
//
// Shutdown: the destructor drains. Every task scheduled before the destructor was
// entered, and every task those tasks spawn, runs to completion before the workers are
// joined. Scheduling from a non-worker thread once the destructor has started, or
// destroying the pool from one of its own workers, is undefined behaviour. So is a
// thread that is still inside a wait on one of the pool's handles or groups while the
// destructor runs (the wait may call into the pool after it is gone); waiting after the
// destructor returned is fine, because every handle is ready by then and a ready handle
// never touches its pool.
class ThreadPool final : public Executor {
 public:
  struct Stats {
    std::uint64_t tasks_executed = 0;  // tasks run by workers (helping waits included)
    std::uint64_t steals = 0;          // tasks taken from another worker's deque
    std::uint64_t injected = 0;        // tasks taken from the injection queue
    std::uint64_t parks = 0;           // times a worker went to sleep
  };

  // Throws std::invalid_argument if num_threads is zero.
  explicit ThreadPool(std::size_t num_threads = default_concurrency());
  ~ThreadPool() override;

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
  void request_exit_and_join() noexcept;
  detail::Task* find_task(Worker& self);
  detail::Task* steal_from_others(Worker& self);
  detail::Task* pop_injected();
  void run_task(Worker& self, detail::Task* task) noexcept;

  // Parking protocol (thread_pool.cpp). Returns a task found while preparing to sleep.
  template <class Done>
  detail::Task* park(Worker& self, bool main_loop, Done&& done);
  void wake_one_if_sleeping();
  void wake_all_locked(std::vector<Worker*>& to_unpark);

  std::vector<std::unique_ptr<Worker>> workers_;

  // Injection queue for tasks from non-worker threads. The counter lets workers skip the
  // lock when the queue is empty; it is accessed seq_cst as part of the parking protocol.
  alignas(kCacheLineSize) std::mutex inject_mutex_;
  std::deque<detail::Task*> injected_;
  std::atomic<std::size_t> injected_count_{0};

  // Sleep state. `sleepers_` counts threads that announced they are about to park (or are
  // parked); producers read it after publishing work and wake someone only if it is
  // non-zero, so the busy path never touches sleep_mutex_.
  alignas(kCacheLineSize) std::atomic<std::size_t> sleepers_{0};
  alignas(kCacheLineSize) std::mutex sleep_mutex_;
  std::atomic<std::uint64_t> epoch_{0};       // bumped (under sleep_mutex_) by every wake
  std::vector<Worker*> idle_;                 // parked workers; guarded by sleep_mutex_
  std::size_t main_idle_ = 0;                 // workers parked in the main loop; guarded
  bool stopping_ = false;                     // destructor entered; guarded
  std::atomic<bool> exiting_{false};          // quiescent after stop: workers leave
};

}  // namespace stealpool
