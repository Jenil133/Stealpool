// Recursive reduction over a randomly shaped (unbalanced) binary tree: one task per
// subtree above a size cutoff. Subtree sizes are unknown up front, which is exactly the
// irregular parallelism work stealing is designed for.
//
//   example_tree_sum [nodes] [threads]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

#include "stealpool/stealpool.hpp"

namespace {

struct Node {
  std::uint64_t value = 0;
  std::size_t size = 1;  // nodes in this subtree
  Node* left = nullptr;
  Node* right = nullptr;
};

// Builds a tree of `count` nodes into `arena` iteratively (random leaf insertion), so a
// degenerate shape cannot overflow the stack while building.
Node* build_random_tree(std::vector<Node>& arena, std::size_t count, std::mt19937_64& rng) {
  arena.resize(count);
  if (count == 0) return nullptr;
  Node* root = &arena[0];
  root->value = rng() % 1000;
  for (std::size_t i = 1; i < count; ++i) {
    Node* node = &arena[i];
    node->value = rng() % 1000;
    Node* at = root;
    for (;;) {
      ++at->size;
      Node*& child = (rng() & 1) ? at->left : at->right;
      if (child == nullptr) {
        child = node;
        break;
      }
      at = child;
    }
  }
  return root;
}

std::uint64_t sum_sequential(const Node* node) {
  std::uint64_t total = 0;
  std::vector<const Node*> stack;
  if (node != nullptr) stack.push_back(node);
  while (!stack.empty()) {
    const Node* n = stack.back();
    stack.pop_back();
    total += n->value;
    if (n->left != nullptr) stack.push_back(n->left);
    if (n->right != nullptr) stack.push_back(n->right);
  }
  return total;
}

std::uint64_t sum_parallel(stealpool::Executor& executor, const Node* node) {
  if (node == nullptr) return 0;
  if (node->size < 1024) return sum_sequential(node);
  std::uint64_t left = 0;
  stealpool::TaskGroup group(executor);
  group.spawn([&] { left = sum_parallel(executor, node->left); });
  const std::uint64_t right = sum_parallel(executor, node->right);
  group.wait();
  return node->value + left + right;
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t nodes = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1'000'000;
  const std::size_t threads =
      argc > 2 ? std::strtoul(argv[2], nullptr, 10) : stealpool::ThreadPool::default_concurrency();

  std::vector<Node> arena;
  std::mt19937_64 rng(7);
  const Node* root = build_random_tree(arena, nodes, rng);
  const std::uint64_t expected = sum_sequential(root);

  stealpool::ThreadPool pool(threads);
  const std::uint64_t total = pool.submit([&] { return sum_parallel(pool, root); }).get();

  const auto stats = pool.stats();
  std::printf("tree of %zu nodes: sum %llu on %zu threads (tasks %llu, steals %llu)\n", nodes,
              static_cast<unsigned long long>(total), threads,
              static_cast<unsigned long long>(stats.tasks_executed),
              static_cast<unsigned long long>(stats.steals));
  if (total != expected) {
    std::fprintf(stderr, "mismatch: expected %llu\n", static_cast<unsigned long long>(expected));
    return 1;
  }
  return 0;
}
