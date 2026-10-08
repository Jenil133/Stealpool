#include "stealpool/thread_pool.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <thread>
#include <utility>

namespace stealpool {

// Parking protocol
// ----------------
// A thread that found no work parks as follows:
//   1. sleepers_.fetch_add(1)                          (seq_cst)  announce
//   2. key = epoch_                                    (acquire)
//   3. look for work again and evaluate the wait condition (the deque/injection checks
//      are seq_cst loads)
//   4. lock sleep_mutex_; if epoch_ != key, give up; otherwise join idle_ and unlock
//   5. block on its own Parker;
//   6. on waking, lock sleep_mutex_ and leave idle_; if a producer had already removed
//      it, look for work once more before returning.
// A producer publishes work with a seq_cst store (deque bottom_ / injected_count_) and
// then reads sleepers_ (seq_cst). Because all four accesses are seq_cst, in the single
// total order either the producer's read sees the announcement (and it wakes a
// sleeper), or the sleeper's re-check in step 3 sees the work. A wake bumps epoch_ under
// sleep_mutex_ and unparks one member of idle_: a sleeper between steps 3 and 4 sees the
// changed epoch, a sleeper past step 4 is in idle_ (or receives the unpark directly), and
// Parker remembers an unpark that arrives before park().
//
// A helping waiter can also be woken by its completed event while it is still listed in
// idle_. A producer that pops it in that window spends its only wakeup on a thread that
// is already awake and about to leave its wait. So removal from idle_ by a producer
// carries an obligation: after waking, a worker that finds itself no longer listed
// searches for work once more (step 6) instead of returning empty-handed. Hence no lost
// wakeups, and no thread ever spins without bound: after a short yield loop it parks.
//
// Waiting for a CompletionEvent on a worker registers the worker's Parker in the event,
// so the thread that completes it unparks exactly that worker (no broadcast).
//
// Termination: after the destructor sets stopping_, a worker that is about to park in its
// main loop counts itself in main_idle_. When all workers are parked in the main loop at
// once, no task is running, every deque is empty (a worker parks only with an empty
// deque and nobody else pushes to it) and the injection queue is empty, so no new task
// can appear: that worker sets exiting_ and wakes everybody.

namespace {

constexpr int kSpinRounds = 32;

struct WorkerContext {
  const void* pool = nullptr;
  void* worker = nullptr;
};
thread_local WorkerContext tls_context;

// Counter written by one thread (its worker) and read by others. A relaxed load + store
// is cheaper than an atomic increment and exact because there is a single writer.
class OwnedCounter {
 public:
  void bump() noexcept { value_.store(value_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
  std::uint64_t get() const noexcept { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<std::uint64_t> value_{0};
};

std::uint64_t splitmix64(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

}  // namespace

struct alignas(kCacheLineSize) ThreadPool::Worker {
  explicit Worker(std::size_t worker_index) : index(worker_index), rng(splitmix64(worker_index + 1)) {}

  // xorshift64: victim selection only needs to be cheap and not identical across workers.
  std::uint64_t next_random() noexcept {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  }

  const std::size_t index;
  WorkStealingDeque<detail::Task*> deque;
  detail::Parker parker;
  std::uint64_t rng;  // owner only
  OwnedCounter executed;
  OwnedCounter steals;
  OwnedCounter injected;
  OwnedCounter parks;
  std::thread thread;
};

ThreadPool::ThreadPool(std::size_t num_threads) {
  if (num_threads == 0) throw std::invalid_argument("ThreadPool needs at least one worker");
  workers_.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) workers_.push_back(std::make_unique<Worker>(i));
  // workers_ is complete before any thread starts and never changes afterwards, so
  // workers may read it without synchronisation.
  try {
    for (auto& worker : workers_) {
      Worker* w = worker.get();
      w->thread = std::thread([this, w] { worker_main(*w); });
    }
  } catch (...) {
    request_exit_and_join();
    throw;
  }
}

ThreadPool::~ThreadPool() {
  assert(this_worker() == nullptr && "a ThreadPool cannot be destroyed by one of its own workers");
  std::vector<Worker*> to_unpark;
  {
    std::lock_guard lock(sleep_mutex_);
    stopping_ = true;
    // Parked workers must re-evaluate: the last one to park again detects quiescence.
    wake_all_locked(to_unpark);
  }
  for (Worker* w : to_unpark) w->parker.unpark();
  for (auto& worker : workers_) worker->thread.join();
  assert(injected_.empty());
}

// Constructor failure path: no task can exist yet, so exit immediately.
void ThreadPool::request_exit_and_join() noexcept {
  std::vector<Worker*> to_unpark;
  {
    std::lock_guard lock(sleep_mutex_);
    stopping_ = true;
    exiting_.store(true, std::memory_order_release);
    wake_all_locked(to_unpark);
  }
  for (Worker* w : to_unpark) w->parker.unpark();
  for (auto& worker : workers_) {
    if (worker->thread.joinable()) worker->thread.join();
  }
}

ThreadPool::Worker* ThreadPool::this_worker() const noexcept {
  return tls_context.pool == this ? static_cast<Worker*>(tls_context.worker) : nullptr;
}

std::optional<std::size_t> ThreadPool::current_worker_index() const noexcept {
  if (Worker* w = this_worker()) return w->index;
  return std::nullopt;
}

std::size_t ThreadPool::default_concurrency() noexcept {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1 : n;
}

ThreadPool::Stats ThreadPool::stats() const noexcept {
  Stats s;
  for (const auto& w : workers_) {
    s.tasks_executed += w->executed.get();
    s.steals += w->steals.get();
    s.injected += w->injected.get();
    s.parks += w->parks.get();
  }
  return s;
}

void ThreadPool::schedule(detail::Task* task) {
  if (Worker* self = this_worker()) {
    // Owner push; its bottom_ store is seq_cst (see the parking protocol).
    self->deque.push(task);
  } else {
    std::lock_guard lock(inject_mutex_);
    injected_.push_back(task);
    injected_count_.fetch_add(1, std::memory_order_seq_cst);
  }
  wake_one_if_sleeping();
}

void ThreadPool::wake_one_if_sleeping() {
  // seq_cst: the load half of the store->load pattern described above.
  if (sleepers_.load(std::memory_order_seq_cst) == 0) return;
  Worker* target = nullptr;
  {
    std::lock_guard lock(sleep_mutex_);
    // release: a sleeper that reads the new epoch (acquire) also sees the published task.
    epoch_.fetch_add(1, std::memory_order_release);
    if (!idle_.empty()) {
      target = idle_.back();
      idle_.pop_back();
    }
  }
  if (target != nullptr) target->parker.unpark();
}

void ThreadPool::wake_all_locked(std::vector<Worker*>& to_unpark) {
  epoch_.fetch_add(1, std::memory_order_release);
  to_unpark.insert(to_unpark.end(), idle_.begin(), idle_.end());
  idle_.clear();
}

detail::Task* ThreadPool::find_task(Worker& self) {
  if (auto task = self.deque.pop()) return *task;
  if (detail::Task* task = steal_from_others(self)) return task;
  detail::Task* task = pop_injected();
  if (task != nullptr) self.injected.bump();
  return task;
}

detail::Task* ThreadPool::steal_from_others(Worker& self) {
  const std::size_t n = workers_.size();
  if (n < 2) return nullptr;
  const std::size_t start = static_cast<std::size_t>(self.next_random() % n);
  for (std::size_t i = 0; i < n; ++i) {
    Worker& victim = *workers_[(start + i) % n];
    if (&victim == &self) continue;
    detail::Task* task = nullptr;
    using Status = WorkStealingDeque<detail::Task*>::StealStatus;
    Status status;
    // A lost race means another thread took an element; the victim may still have more,
    // and treating it as empty could let this thread park while work is available.
    while ((status = victim.deque.try_steal(task)) == Status::kLostRace) {
    }
    if (status == Status::kSuccess) {
      self.steals.bump();
      return task;
    }
  }
  return nullptr;
}

detail::Task* ThreadPool::pop_injected() {
  // seq_cst: pairs with the producer's seq_cst increment in the parking protocol.
  if (injected_count_.load(std::memory_order_seq_cst) == 0) return nullptr;
  std::lock_guard lock(inject_mutex_);
  if (injected_.empty()) return nullptr;
  detail::Task* task = injected_.front();
  injected_.pop_front();
  injected_count_.fetch_sub(1, std::memory_order_relaxed);
  return task;
}

void ThreadPool::run_task(Worker& self, detail::Task* task) noexcept {
  // Counted before running, so a thread that observed the task's completion also sees it
  // counted (the completion is a release that happens after this store).
  self.executed.bump();
  task->execute();
}

template <class Done>
detail::Task* ThreadPool::park(Worker& self, bool main_loop, Done&& done) {
  sleepers_.fetch_add(1, std::memory_order_seq_cst);
  struct Unannounce {
    std::atomic<std::size_t>& sleepers;
    ~Unannounce() { sleepers.fetch_sub(1, std::memory_order_relaxed); }
  } unannounce{sleepers_};

  const std::uint64_t key = epoch_.load(std::memory_order_acquire);
  if (detail::Task* task = find_task(self)) return task;
  if (done()) return nullptr;

  {
    std::unique_lock lock(sleep_mutex_);
    if (epoch_.load(std::memory_order_relaxed) != key || exiting_.load(std::memory_order_relaxed)) {
      return nullptr;
    }
    if (main_loop) {
      ++main_idle_;
      if (stopping_ && main_idle_ == workers_.size() &&
          injected_count_.load(std::memory_order_relaxed) == 0) {
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
  }

  self.parks.bump();
  self.parker.park();

  bool listed;
  {
    std::lock_guard lock(sleep_mutex_);
    // Still listed if the unpark came from somewhere other than a wake (a completed
    // event, or a stale unpark from an earlier wait).
    auto it = std::find(idle_.begin(), idle_.end(), &self);
    listed = it != idle_.end();
    if (listed) idle_.erase(it);
    if (main_loop) --main_idle_;
  }
  // Not listed: a producer removed this worker from idle_ and spent its single wakeup on
  // it. The worker may have been woken by its completed event instead and would then
  // leave its wait without looking for that producer's task, while the workers that are
  // really parked stay asleep. Taking over the producer's obligation to search keeps the
  // wakeup from being lost.
  if (!listed) return find_task(self);
  return nullptr;
}

void ThreadPool::worker_main(Worker& self) {
  tls_context = WorkerContext{this, &self};
  for (;;) {
    detail::Task* task = find_task(self);
    for (int round = 0; task == nullptr && round < kSpinRounds; ++round) {
      std::this_thread::yield();
      task = find_task(self);
    }
    if (task == nullptr) {
      task = park(self, /*main_loop=*/true, [this] { return exiting_.load(std::memory_order_acquire); });
    }
    if (task != nullptr) {
      run_task(self, task);
    } else if (exiting_.load(std::memory_order_acquire)) {
      break;
    }
  }
  tls_context = WorkerContext{};
}

void ThreadPool::wait_for(detail::CompletionEvent& event) {
  Worker* self = this_worker();
  if (self == nullptr) {
    // Not one of our workers: block. If registration succeeds, the setter will unpark
    // this Parker exactly once, so it is safe to keep it on the stack.
    detail::Parker parker;
    if (event.try_register(&parker)) parker.park();
    return;
  }

  // Help: run other tasks until the event is set. The worker's Parker is registered only
  // right before it first parks. In fork-join the waiter usually runs its own child, and
  // an eager registration would make that child's completion unpark the waiter's own
  // Parker, leaving a stale permit that cuts its next sleep short. Registering before
  // park() re-checks the event keeps the wakeup targeted and impossible to lose.
  bool registered = false;
  while (!event.is_set()) {
    detail::Task* task = find_task(*self);
    for (int round = 0; task == nullptr && round < kSpinRounds && !event.is_set(); ++round) {
      std::this_thread::yield();
      task = find_task(*self);
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
