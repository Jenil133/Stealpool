// Workloads used by the scaling benchmarks. They are templated over the pool type, so the
// same code runs on the work-stealing ThreadPool and on the SharedQueuePool baseline, and
// they are checked for correctness by tests/shared_queue_pool_test.cpp.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include "stealpool/executor.hpp"
#include "stealpool/task_group.hpp"

namespace stealpool::bench {

template <class Pool>
concept PoolType = std::is_base_of_v<Executor, Pool>;

// ---------------------------------------------------------------------------------------
// Parallel merge sort
//
// Both the recursive sort and the merge are parallel, so the critical path is
// polylogarithmic and the benchmark measures the scheduler rather than a sequential
// top-level partition or merge. Below the cutoffs the work is sequential (std::sort,
// std::merge).

struct SortParams {
  std::size_t sort_cutoff = 8192;   // elements sorted sequentially
  std::size_t merge_cutoff = 8192;  // output elements merged sequentially
};

namespace detail {

// Merges sorted [a, a_end) and [b, b_end) into `out`. The longer input is split at its
// middle element; a binary search splits the other one, so every element of the left
// half of the output is <= every element of the right half.
template <PoolType Pool, class T>
void parallel_merge(Pool& pool, const T* a, const T* a_end, const T* b, const T* b_end, T* out,
                    const SortParams& params) {
  const auto a_len = static_cast<std::size_t>(a_end - a);
  const auto b_len = static_cast<std::size_t>(b_end - b);
  if (a_len + b_len <= params.merge_cutoff) {
    std::merge(a, a_end, b, b_end, out);
    return;
  }
  if (a_len < b_len) {
    std::swap(a, b);
    std::swap(a_end, b_end);
  }
  const T* a_mid = a + (a_end - a) / 2;
  const T* b_mid = std::lower_bound(b, b_end, *a_mid);
  T* out_mid = out + (a_mid - a) + (b_mid - b);
  TaskGroup group(pool);
  group.spawn([&pool, a, a_mid, b, b_mid, out, &params] { parallel_merge(pool, a, a_mid, b, b_mid, out, params); });
  parallel_merge(pool, a_mid, a_end, b_mid, b_end, out_mid, params);
  group.wait();
}

// Sorts data[0, n). `buffer` is scratch space of the same size. With result_in_buffer the
// sorted sequence ends up in buffer instead of data (ping-pong between the two arrays
// avoids a copy after every merge).
template <PoolType Pool, class T>
void parallel_merge_sort(Pool& pool, T* data, T* buffer, std::size_t n, bool result_in_buffer,
                         const SortParams& params) {
  if (n <= params.sort_cutoff) {
    std::sort(data, data + n);
    if (result_in_buffer) std::copy(data, data + n, buffer);
    return;
  }
  const std::size_t half = n / 2;
  {
    TaskGroup group(pool);
    group.spawn([&pool, data, buffer, half, result_in_buffer, &params] {
      parallel_merge_sort(pool, data, buffer, half, !result_in_buffer, params);
    });
    parallel_merge_sort(pool, data + half, buffer + half, n - half, !result_in_buffer, params);
    group.wait();
  }
  // The sorted halves are in the array the result does not go to.
  const T* src = result_in_buffer ? data : buffer;
  T* dst = result_in_buffer ? buffer : data;
  parallel_merge(pool, src, src + half, src + half, src + n, dst, params);
}

}  // namespace detail

// Sorts `values` in place; `scratch` is resized as needed and may be reused across calls.
// Call from a worker of `pool` (for example inside pool.submit()) so waits help.
template <PoolType Pool, class T>
void parallel_sort(Pool& pool, std::vector<T>& values, std::vector<T>& scratch, const SortParams& params = {}) {
  scratch.resize(values.size());
  detail::parallel_merge_sort(pool, values.data(), scratch.data(), values.size(), /*result_in_buffer=*/false,
                              params);
}

inline std::vector<int> random_ints(std::size_t count, std::uint64_t seed) {
  std::vector<int> values(count);
  std::mt19937_64 rng(seed);
  for (int& v : values) v = static_cast<int>(rng());
  return values;
}

// ---------------------------------------------------------------------------------------
// Unbalanced tree reduction
//
// The tree is stored in preorder in one array. A node of subtree size s puts
// floor((s - 1) * u^2) nodes (u uniform in [0, 1)) in its left subtree and the rest in the
// right one, so sibling subtrees routinely differ by orders of magnitude and the tree is
// much deeper than a balanced one. Nobody knows subtree sizes when splitting work at run
// time without looking, which is the irregular parallelism that stealing is meant for.

struct TreeNode {
  std::uint64_t value = 0;
  std::uint32_t size = 1;  // nodes in this subtree, used only to pick the sequential cutoff
  std::int32_t left = -1;  // child indices into the array, -1 when absent
  std::int32_t right = -1;
};

struct Tree {
  std::vector<TreeNode> nodes;
  std::size_t depth = 0;  // longest root-to-leaf path, in nodes
};

inline Tree build_unbalanced_tree(std::size_t count, std::uint64_t seed) {
  Tree tree;
  tree.nodes.resize(count);
  if (count == 0) return tree;
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> uniform(0.0, 1.0);

  struct Pending {
    std::size_t index;
    std::size_t size;
    std::size_t depth;
  };
  std::vector<Pending> stack{{0, count, 1}};
  while (!stack.empty()) {
    const Pending p = stack.back();
    stack.pop_back();
    TreeNode& node = tree.nodes[p.index];
    node.value = rng() % 1000;
    node.size = static_cast<std::uint32_t>(p.size);
    tree.depth = std::max(tree.depth, p.depth);
    const double u = uniform(rng);
    const auto left_size = std::min(p.size - 1, static_cast<std::size_t>(static_cast<double>(p.size - 1) * u * u));
    const std::size_t right_size = p.size - 1 - left_size;
    if (left_size > 0) {
      node.left = static_cast<std::int32_t>(p.index + 1);
      stack.push_back({p.index + 1, left_size, p.depth + 1});
    }
    if (right_size > 0) {
      node.right = static_cast<std::int32_t>(p.index + 1 + left_size);
      stack.push_back({p.index + 1 + left_size, right_size, p.depth + 1});
    }
  }
  return tree;
}

// Simulated per-node computation: `rounds` steps of a 64-bit mixing function. The result
// feeds the reduction, so the compiler cannot drop it.
inline std::uint64_t node_work(std::uint64_t value, unsigned rounds) noexcept {
  std::uint64_t x = value + 0x9e3779b97f4a7c15ULL;
  for (unsigned i = 0; i < rounds; ++i) {
    x ^= x >> 31;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 29;
  }
  return x;
}

struct TreeParams {
  unsigned work_rounds = 64;          // node_work() rounds per node
  std::uint32_t sequential_cutoff = 256;  // subtrees smaller than this are reduced inline
};

inline std::uint64_t reduce_tree_sequential(const Tree& tree, std::int32_t root, unsigned work_rounds) {
  if (root < 0) return 0;
  std::uint64_t total = 0;
  std::vector<std::int32_t> stack{root};
  while (!stack.empty()) {
    const TreeNode& n = tree.nodes[static_cast<std::size_t>(stack.back())];
    stack.pop_back();
    total += node_work(n.value, work_rounds);
    if (n.left >= 0) stack.push_back(n.left);
    if (n.right >= 0) stack.push_back(n.right);
  }
  return total;
}

// Fork-join reduction. A task walks down the right spine of its subtree on its own thread
// and spawns every sizeable left subtree as a child task, until the rest of the spine is
// small enough to reduce inline. Walking instead of recursing keeps each task's stack
// use constant however deep the tree is; that matters because a helping wait runs other
// tasks on top of the waiter's stack.
template <PoolType Pool>
std::uint64_t reduce_tree(Pool& pool, const Tree& tree, std::int32_t root, const TreeParams& params) {
  std::uint64_t total = 0;
  // Children add their results here; group.wait() makes their additions visible.
  std::atomic<std::uint64_t> from_children{0};
  TaskGroup group(pool);
  std::int32_t at = root;
  while (at >= 0 && tree.nodes[static_cast<std::size_t>(at)].size >= params.sequential_cutoff) {
    const TreeNode& node = tree.nodes[static_cast<std::size_t>(at)];
    if (node.left >= 0 && tree.nodes[static_cast<std::size_t>(node.left)].size < params.sequential_cutoff) {
      total += reduce_tree_sequential(tree, node.left, params.work_rounds);
    } else if (node.left >= 0) {
      group.spawn([&pool, &tree, &params, &from_children, left = node.left] {
        from_children.fetch_add(reduce_tree(pool, tree, left, params), std::memory_order_relaxed);
      });
    }
    total += node_work(node.value, params.work_rounds);
    at = node.right;
  }
  total += reduce_tree_sequential(tree, at, params.work_rounds);
  group.wait();
  return total + from_children.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------
// Task overhead
//
// A binary fork-join tree of empty tasks: 2^depth - 1 spawns, each doing nothing but
// being scheduled, run and joined. Returns the number of tasks executed so callers can
// check that none was lost.

template <PoolType Pool>
std::uint64_t spawn_tree(Pool& pool, unsigned depth) {
  if (depth == 0) return 0;
  std::uint64_t left = 0;
  TaskGroup group(pool);
  group.spawn([&pool, &left, depth] { left = spawn_tree(pool, depth - 1); });
  const std::uint64_t right = spawn_tree(pool, depth - 1);
  group.wait();
  return left + right + 1;
}

}  // namespace stealpool::bench
