// The executor interface shared by every pool, and submit()/TaskHandle on top of it.
//
// Pools differ only in how they queue tasks and how a thread waits. Everything else
// (task objects, handles, task groups, parallel algorithms) is written once against
// this interface, so a benchmark can swap the work-stealing pool for a baseline pool
// without touching the algorithm being measured.
#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include "stealpool/detail/task.hpp"

namespace stealpool {

template <class R>
class TaskHandle;

class Executor {
 public:
  Executor() = default;
  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;
  virtual ~Executor() = default;

  // Number of threads that execute tasks.
  virtual std::size_t concurrency() const noexcept = 0;

  // Runs `fn()` asynchronously and returns a handle to its result. Exceptions thrown by
  // `fn` are captured and rethrown by TaskHandle::get(). Dropping the handle detaches the
  // task (it still runs; its result is discarded).
  template <class F>
  [[nodiscard]] auto submit(F&& fn) -> TaskHandle<std::invoke_result_t<std::decay_t<F>&>>;

  // ---- Low-level interface used by TaskHandle, TaskGroup and the algorithms. ----

  // Makes `task` runnable. Ownership passes to the executor, which calls
  // task->execute() exactly once.
  virtual void schedule(detail::Task* task) = 0;

  // Returns once `event` is set. When called from one of this executor's worker threads
  // the caller keeps executing other tasks while it waits ("helping"), so fork-join code
  // cannot deadlock even with a single worker. Other threads block. At most one thread
  // may wait on a given event.
  virtual void wait_for(detail::CompletionEvent& event) = 0;
};

namespace detail {

template <class R>
struct ResultSlot {
  std::optional<R> value;
};
template <>
struct ResultSlot<void> {};

// Shared state between a submitted task and its handle. It is the task object itself,
// so submit() costs one allocation. Two references: the executing side drops its
// reference after publishing the result, the handle drops its own when it is
// destroyed (or after get()).
template <class R>
class SharedResult : public Task {
 public:
  void release() noexcept {
    // acq_rel: the last owner must see every write made by the other owner before it
    // frees the object.
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) destroy_(this);
  }

  bool is_ready() const noexcept { return done_.is_set(); }
  void wait() {
    if (!done_.is_set()) executor_->wait_for(done_);
  }

  R take() {
    // Ordering: wait() observed done_ set (acquire), which pairs with the release in
    // complete(), so `slot_` and `error_` are fully written.
    //
    // The exception is moved out so that this thread holds its last reference. Otherwise
    // the executing side, if it drops its reference to the shared state last, would
    // destroy the exception object on its own thread after the caller has used it,
    // ordered only by the standard library's internal reference count. ThreadSanitizer
    // cannot see that ordering when the library is not instrumented (libstdc++ on Linux)
    // and reports a race on the exception's message.
    if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
    if constexpr (!std::is_void_v<R>) return std::move(*slot_.value);
  }

 protected:
  using DestroyFn = void (*)(SharedResult*) noexcept;

  SharedResult(RunFn run, DestroyFn destroy, Executor& executor) noexcept
      : Task(run), destroy_(destroy), executor_(&executor) {}
  ~SharedResult() = default;

  void complete() noexcept {
    if (Parker* waiter = done_.set()) waiter->unpark();
  }

  ResultSlot<R> slot_;
  std::exception_ptr error_;

 private:
  std::atomic<int> refs_{2};
  CompletionEvent done_;
  DestroyFn destroy_;
  Executor* executor_;
};

template <class R, class F>
class SubmittedTask final : public SharedResult<R> {
 public:
  template <class G>
  SubmittedTask(G&& fn, Executor& executor)
      : SharedResult<R>(&run, &destroy, executor), fn_(std::in_place, std::forward<G>(fn)) {}

 private:
  static void run(Task* base) noexcept {
    auto* self = static_cast<SubmittedTask*>(base);
    try {
      if constexpr (std::is_void_v<R>) {
        std::invoke(*self->fn_);
      } else {
        self->slot_.value.emplace(std::invoke(*self->fn_));
      }
    } catch (...) {
      self->error_ = std::current_exception();
    }
    // Release captured state before the waiter can resume, so a caller that waits on the
    // handle may rely on the callable's captures being gone.
    self->fn_.reset();
    self->complete();
    self->release();
  }

  static void destroy(SharedResult<R>* base) noexcept { delete static_cast<SubmittedTask*>(base); }

  std::optional<F> fn_;
};

}  // namespace detail

// Move-only handle to the result of Executor::submit(), modelled on std::future.
// A handle is used by one thread at a time: unlike most const member functions, wait()
// must not be called concurrently from two threads (the completion event has room for a
// single waiter; debug builds assert).
//
// Waiting on a worker thread helps: other queued tasks run nested on the waiter's stack.
// A task therefore must not block on another task's continuation (code that runs after
// some other task's wait()/get() returns), and must not hold a lock across wait()/get(),
// or the pool can deadlock even with idle workers.
template <class R>
class [[nodiscard]] TaskHandle {
  static_assert(!std::is_reference_v<R>, "tasks must return values, not references");

 public:
  TaskHandle() noexcept = default;
  TaskHandle(TaskHandle&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}
  TaskHandle& operator=(TaskHandle&& other) noexcept {
    if (this != &other) {
      reset();
      state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
  }
  ~TaskHandle() { reset(); }

  bool valid() const noexcept { return state_ != nullptr; }
  bool is_ready() const noexcept { return state_->is_ready(); }

  // Waits for completion without consuming the result. Helps on a worker thread. Single
  // waiter only, see above.
  void wait() const { state_->wait(); }

  // Waits, then returns the result or rethrows the task's exception. Invalidates the
  // handle, like std::future::get().
  R get() {
    state_->wait();
    struct Release {
      TaskHandle* self;
      ~Release() { self->reset(); }
    } release{this};
    return state_->take();
  }

 private:
  friend class Executor;
  explicit TaskHandle(detail::SharedResult<R>* state) noexcept : state_(state) {}

  void reset() noexcept {
    if (state_ != nullptr) std::exchange(state_, nullptr)->release();
  }

  detail::SharedResult<R>* state_ = nullptr;
};

template <class F>
auto Executor::submit(F&& fn) -> TaskHandle<std::invoke_result_t<std::decay_t<F>&>> {
  using R = std::invoke_result_t<std::decay_t<F>&>;
  auto* task = new detail::SubmittedTask<R, std::decay_t<F>>(std::forward<F>(fn), *this);
  TaskHandle<R> handle(task);
  try {
    schedule(task);
  } catch (...) {
    task->release();  // the executing side's reference; the handle releases the other
    throw;
  }
  return handle;
}

}  // namespace stealpool
