#include "stealpool/shared_queue_pool.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <thread>
#include <utility>

namespace stealpool {

// Sleeping and waking
// -------------------
// Every decision to sleep is made while holding mutex_, the same lock that guards the
// queue: a worker re-checks the queue (and, in a helping wait, its event) under the lock,
// then adds itself to idle_ before unlocking and parking. A producer pushes under the lock
// and pops one idle worker to unpark. So a task is either seen by the re-check or its
// producer finds the worker in idle_; no wakeup can be lost. Events are signalled through
// the waiter's Parker, which remembers a signal that arrives before park().
//
// Which task a worker takes: its own newest queued task if it has one, otherwise the
// oldest task in the queue. That is ThreadPool's order with every per-worker deque merged
// into one: a worker runs its own most recent child first (LIFO, cache-hot,
// recursion-shaped) and only otherwise takes the oldest task, the largest piece of a
// fork-join computation, like a thief. Each worker counts its queued entries so the
// search from the back is skipped when it has none.
//
// The rule matters for helping waits, which run the task on top of the waiter's stack.
// Simpler rules made the stack grow far beyond the recursion depth:
//   * always the oldest: the waiter's task waits and takes the next oldest, and so on, so
//     the nesting grows with the number of queued tasks (fib(20) on one worker overflowed
//     a worker thread's stack);
//   * always the newest: two waiting workers keep taking each other's newest child
//     (fib(20) on two workers overflowed under ASan);
//   * the newest only if this worker pushed it, else the oldest: with several workers
//     spawning, the back is usually someone else's entry, so waiters took over large old
//     tasks far more often than ThreadPool steals (fib(20) still overflowed under ASan).
//
// Termination works as in ThreadPool: once the destructor has set stopping_, the worker
// that brings main_idle_ to the worker count has proved that no task is running and the
// queue is empty, so it sets exiting_ and wakes everybody.

namespace {

// Same idle policy as ThreadPool, so the two pools differ only in their queues.
constexpr int kSpinRounds = 32;

struct WorkerContext {
  const void* pool = nullptr;
  void* worker = nullptr;
};
thread_local WorkerContext tls_context;

// Counter written only by its worker and read by stats().
class OwnedCounter {
 public:
  void bump() noexcept { value_.store(value_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
  std::uint64_t get() const noexcept { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<std::uint64_t> value_{0};
};

}  // namespace

struct alignas(kCacheLineSize) SharedQueuePool::Worker {
  explicit Worker(std::size_t worker_index) : index(worker_index) {}

  const std::size_t index;
  std::size_t queued = 0;  // entries this worker pushed that are still queued; guarded by mutex_
  detail::Parker parker;
  OwnedCounter executed;
  OwnedCounter parks;
  std::thread thread;
};

SharedQueuePool::SharedQueuePool(std::size_t num_threads) {
  if (num_threads == 0) throw std::invalid_argument("SharedQueuePool needs at least one worker");
  workers_.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) workers_.push_back(std::make_unique<Worker>(i));
  try {
    for (auto& worker : workers_) {
      Worker* w = worker.get();
      w->thread = std::thread([this, w] { worker_main(*w); });
    }
  } catch (...) {
    stop_and_join();
    throw;
  }
}

SharedQueuePool::~SharedQueuePool() {
  assert(this_worker() == nullptr && "a SharedQueuePool cannot be destroyed by one of its own workers");
  std::vector<Worker*> to_unpark;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    // Parked workers must re-evaluate: the last one to park again detects quiescence.
    wake_all_locked(to_unpark);
  }
  for (Worker* w : to_unpark) w->parker.unpark();
  for (auto& worker : workers_) worker->thread.join();
  assert(queue_.empty());
}

// Constructor failure path: no task can exist yet, so exit immediately.
void SharedQueuePool::stop_and_join() noexcept {
  std::vector<Worker*> to_unpark;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    exiting_.store(true, std::memory_order_release);
    wake_all_locked(to_unpark);
  }
  for (Worker* w : to_unpark) w->parker.unpark();
  for (auto& worker : workers_) {
    if (worker->thread.joinable()) worker->thread.join();
  }
}

SharedQueuePool::Worker* SharedQueuePool::this_worker() const noexcept {
  return tls_context.pool == this ? static_cast<Worker*>(tls_context.worker) : nullptr;
}

std::optional<std::size_t> SharedQueuePool::current_worker_index() const noexcept {
  if (Worker* w = this_worker()) return w->index;
  return std::nullopt;
}

std::size_t SharedQueuePool::default_concurrency() noexcept {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1 : n;
}

SharedQueuePool::Stats SharedQueuePool::stats() const noexcept {
  Stats s;
  for (const auto& w : workers_) {
    s.tasks_executed += w->executed.get();
    s.parks += w->parks.get();
  }
  return s;
}

void SharedQueuePool::schedule(detail::Task* task) {
  Worker* target = nullptr;
  {
    std::lock_guard lock(mutex_);
    Worker* self = this_worker();
    queue_.push_back(Entry{task, self != nullptr ? self->index : kExternal});
    if (self != nullptr) ++self->queued;
    size_.store(queue_.size(), std::memory_order_relaxed);
    if (!idle_.empty()) {
      target = idle_.back();
      idle_.pop_back();
    }
  }
  if (target != nullptr) target->parker.unpark();
}

void SharedQueuePool::wake_all_locked(std::vector<Worker*>& to_unpark) {
  to_unpark.insert(to_unpark.end(), idle_.begin(), idle_.end());
  idle_.clear();
}

detail::Task* SharedQueuePool::pop_locked(Worker& self) {
  if (queue_.empty()) return nullptr;
  detail::Task* task = nullptr;
  if (self.queued > 0) {
    // This worker's newest entry. It is usually at or near the back: only tasks pushed by
    // other threads since this worker's last spawn lie behind it.
    auto it = queue_.end();
    do {
      --it;
    } while (it->pusher != self.index);
    task = it->task;
    queue_.erase(it);
    --self.queued;
  } else {
    const Entry oldest = queue_.front();
    queue_.pop_front();
    if (oldest.pusher != kExternal) --workers_[oldest.pusher]->queued;
    task = oldest.task;
  }
  size_.store(queue_.size(), std::memory_order_relaxed);
  return task;
}

detail::Task* SharedQueuePool::try_pop(Worker& self) {
  // A stale zero only delays this worker until it re-checks under the lock in park().
  if (size_.load(std::memory_order_relaxed) == 0) return nullptr;
  std::lock_guard lock(mutex_);
  return pop_locked(self);
}

void SharedQueuePool::run_task(Worker& self, detail::Task* task) noexcept {
  self.executed.bump();
  task->execute();
}

template <class Done>
detail::Task* SharedQueuePool::park(Worker& self, bool main_loop, Done&& done) {
  std::unique_lock lock(mutex_);
  if (detail::Task* task = pop_locked(self)) return task;
  if (done() || exiting_.load(std::memory_order_relaxed)) return nullptr;
  if (main_loop) {
    ++main_idle_;
    // The queue was just found empty under the lock and every other worker is parked in
    // its main loop, so nothing is running that could schedule more work.
    if (stopping_ && main_idle_ == workers_.size()) {
      --main_idle_;
      exiting_.store(true, std::memory_order_release);
      std::vector<Worker*> to_unpark;
      wake_all_locked(to_unpark);
      lock.unlock();
      for (Worker* w : to_unpark) w->parker.unpark();
      return nullptr;
    }
  }
  idle_.push_back(&self);
  lock.unlock();

  self.parks.bump();
  self.parker.park();

  lock.lock();
  // Still listed if the unpark came from a completed event (or a stale unpark from an
  // earlier wait) rather than from a producer. Either way the queue is checked again under
  // the lock, so a producer's wakeup spent on a waiter that its event had already woken is
  // not lost: that waiter takes the producer's task.
  auto it = std::find(idle_.begin(), idle_.end(), &self);
  if (it != idle_.end()) idle_.erase(it);
  if (main_loop) --main_idle_;
  return pop_locked(self);
}

void SharedQueuePool::worker_main(Worker& self) {
  tls_context = WorkerContext{this, &self};
  for (;;) {
    detail::Task* task = try_pop(self);
    for (int round = 0; task == nullptr && round < kSpinRounds; ++round) {
      std::this_thread::yield();
      task = try_pop(self);
    }
    if (task == nullptr) task = park(self, /*main_loop=*/true, [] { return false; });
    if (task != nullptr) {
      run_task(self, task);
    } else if (exiting_.load(std::memory_order_acquire)) {
      break;
    }
  }
  tls_context = WorkerContext{};
}

void SharedQueuePool::wait_for(detail::CompletionEvent& event) {
  Worker* self = this_worker();
  if (self == nullptr) {
    // Not one of our workers: block. A successful registration guarantees exactly one
    // unpark from the setter, so the Parker may live on the stack.
    detail::Parker parker;
    if (event.try_register(&parker)) parker.park();
    return;
  }

  // Help: run queued tasks until the event is set. As in ThreadPool, the Parker is
  // registered lazily, right before the first park(), so that a child run by the waiter
  // itself does not unpark the waiter's own Parker; park() re-checks the event after the
  // registration, so the setter's targeted wakeup cannot be lost.
  bool registered = false;
  while (!event.is_set()) {
    detail::Task* task = try_pop(*self);
    for (int round = 0; task == nullptr && round < kSpinRounds && !event.is_set(); ++round) {
      std::this_thread::yield();
      task = try_pop(*self);
    }
    if (task == nullptr && !event.is_set()) {
      if (!registered) {
        if (!event.try_register(&self->parker)) return;  // set in the meantime
        registered = true;
      }
      task = park(*self, /*main_loop=*/false, [&event] { return event.is_set(); });
    }
    if (task != nullptr) run_task(*self, task);
  }
}

}  // namespace stealpool
