// Naive recursive Fibonacci: the classic fork-join stress for a scheduler, because the
// task tree is deep and every task waits on a child that may have been stolen.
//
//   example_fib [n] [threads]
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "stealpool/stealpool.hpp"

namespace {

std::uint64_t fib_sequential(unsigned n) { return n < 2 ? n : fib_sequential(n - 1) + fib_sequential(n - 2); }

std::uint64_t fib_parallel(stealpool::Executor& executor, unsigned n) {
  if (n < 12) return fib_sequential(n);  // below this, a task costs more than it saves
  std::uint64_t left = 0;
  stealpool::TaskGroup group(executor);
  group.spawn([&] { left = fib_parallel(executor, n - 1); });
  const std::uint64_t right = fib_parallel(executor, n - 2);
  group.wait();
  return left + right;
}

}  // namespace

int main(int argc, char** argv) {
  const unsigned n = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 30;
  const std::size_t threads =
      argc > 2 ? std::strtoul(argv[2], nullptr, 10) : stealpool::ThreadPool::default_concurrency();

  stealpool::ThreadPool pool(threads);
  const std::uint64_t parallel = pool.submit([&] { return fib_parallel(pool, n); }).get();
  const std::uint64_t expected = fib_sequential(n);
  const auto stats = pool.stats();
  std::printf("fib(%u) = %llu on %zu threads (tasks %llu, steals %llu)\n", n,
              static_cast<unsigned long long>(parallel), threads,
              static_cast<unsigned long long>(stats.tasks_executed),
              static_cast<unsigned long long>(stats.steals));
  if (parallel != expected) {
    std::fprintf(stderr, "mismatch: expected %llu\n", static_cast<unsigned long long>(expected));
    return 1;
  }
  return 0;
}
