// Data-parallel loops built on TaskGroup. They work with any Executor.
#pragma once

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "stealpool/executor.hpp"
#include "stealpool/task_group.hpp"

namespace stealpool {

namespace detail {

// Recursive halving: split off the upper half as a task, keep the lower half, repeat
// until the range is no larger than `grain`, run it, then join. Each worker keeps
// splitting the piece it holds while thieves take the large halves from the top of its
// deque, which is what makes the loop adapt to uneven iteration costs.
template <class Body>
void parallel_for_split(Executor& executor, std::size_t first, std::size_t last, std::size_t grain,
                        const Body& body) {
  TaskGroup group(executor);
  while (last - first > grain) {
    const std::size_t mid = first + (last - first) / 2;
    group.spawn([&executor, mid, last, grain, &body] { parallel_for_split(executor, mid, last, grain, body); });
    last = mid;
  }
  body(first, last);
  group.wait();
}

inline std::size_t default_grain(const Executor& executor, std::size_t count) {
  // About eight chunks per thread: enough slack for stealing to even out imbalance,
  // few enough that per-task overhead stays small.
  const std::size_t chunks = std::max<std::size_t>(1, executor.concurrency() * 8);
  return std::max<std::size_t>(1, count / chunks);
}

}  // namespace detail

// Calls body(lo, hi) on disjoint sub-ranges that exactly cover [first, last), each at most
// `grain` long (grain 0 picks a default). Returns when all calls have finished; the first
// exception thrown by a call is rethrown (remaining chunks still run).
//
// The same body object is called concurrently from several threads, always through a
// const reference, so a mutable lambda (whose captures would be raced on) is rejected.
template <class Body>
void parallel_for_blocked(Executor& executor, std::size_t first, std::size_t last, std::size_t grain,
                          Body&& body) {
  static_assert(std::is_invocable_v<const std::remove_reference_t<Body>&, std::size_t, std::size_t>,
                "body must be const-callable as body(lo, hi); it is called from several threads at once");
  if (last <= first) return;
  if (grain == 0) grain = detail::default_grain(executor, last - first);
  detail::parallel_for_split(executor, first, last, grain, body);
}

// Calls body(i) for every i in [first, last), concurrently from several threads (same
// const-call requirement as parallel_for_blocked).
template <class Body>
void parallel_for(Executor& executor, std::size_t first, std::size_t last, std::size_t grain, Body&& body) {
  static_assert(std::is_invocable_v<const std::remove_reference_t<Body>&, std::size_t>,
                "body must be const-callable as body(i); it is called from several threads at once");
  const auto& shared_body = body;
  parallel_for_blocked(executor, first, last, grain, [&shared_body](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) shared_body(i);
  });
}

template <class Body>
void parallel_for(Executor& executor, std::size_t first, std::size_t last, Body&& body) {
  parallel_for(executor, first, last, 0, std::forward<Body>(body));
}

}  // namespace stealpool
