#include "stealpool/task_graph.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <new>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "stealpool/shared_queue_pool.hpp"
#include "stealpool/task_group.hpp"
#include "stealpool/thread_pool.hpp"

namespace {

using stealpool::GraphCycleError;
using stealpool::GraphTask;
using stealpool::TaskGraph;
using stealpool::ThreadPool;

// Thread counts used by the parameterized tests: one worker (everything runs through
// helping and the owner's deque), a few, and more workers than this test has nodes on
// some levels.
class TaskGraphThreads : public ::testing::TestWithParam<std::size_t> {
 protected:
  ThreadPool pool_{GetParam()};
};

INSTANTIATE_TEST_SUITE_P(Workers, TaskGraphThreads, ::testing::Values(1, 2, 4, 8),
                         [](const auto& info) { return std::to_string(info.param) + "w"; });

TEST(TaskGraph, EmptyGraphCompletesImmediately) {
  ThreadPool pool(2);
  TaskGraph graph;
  EXPECT_TRUE(graph.empty());
  for (int i = 0; i < 3; ++i) {
    auto run = graph.run(pool);
    EXPECT_TRUE(run.is_ready());
    run.wait();
    EXPECT_FALSE(graph.is_running());
  }
}

TEST(TaskGraph, SingleNodeRunsOncePerRun) {
  ThreadPool pool(2);
  TaskGraph graph;
  int runs = 0;
  graph.emplace([&] { ++runs; });
  EXPECT_EQ(graph.size(), 1u);
  graph.run(pool).wait();
  EXPECT_EQ(runs, 1);
  graph.run_and_wait(pool);
  EXPECT_EQ(runs, 2);
}

TEST_P(TaskGraphThreads, LongChainRunsInOrder) {
  constexpr int kLength = 2000;
  TaskGraph graph;
  // Plain (non-atomic) state: each node must happen-after its predecessor, which both the
  // value check and ThreadSanitizer verify.
  int next = 0;
  std::vector<int> order;
  GraphTask previous;
  for (int i = 0; i < kLength; ++i) {
    GraphTask node = graph.emplace([&, i] {
      EXPECT_EQ(next, i);
      ++next;
      order.push_back(i);
    });
    if (previous.valid()) previous.precede(node);
    previous = node;
  }
  graph.run(pool_).wait();
  ASSERT_EQ(next, kLength);
  for (int i = 0; i < kLength; ++i) ASSERT_EQ(order[static_cast<std::size_t>(i)], i);
}

TEST_P(TaskGraphThreads, Diamond) {
  TaskGraph graph;
  int a = 0, b = 0, c = 0, d = 0;
  auto ta = graph.emplace([&] { a = 1; });
  auto tb = graph.emplace([&] { b = a + 1; });
  auto tc = graph.emplace([&] { c = a + 2; });
  auto td = graph.emplace([&] { d = b + c; });
  ta.precede(tb, tc);
  td.succeed(tb, tc);
  EXPECT_EQ(ta.num_successors(), 2u);
  EXPECT_EQ(td.num_predecessors(), 2u);
  for (int round = 0; round < 20; ++round) {
    a = b = c = d = 0;
    graph.run(pool_).wait();
    ASSERT_EQ(d, 5);
  }
}

TEST_P(TaskGraphThreads, WideFanOutThenFanIn) {
  constexpr std::size_t kWidth = 3000;
  TaskGraph graph;
  int source_value = 0;
  std::vector<int> leaf(kWidth, 0);
  std::int64_t sum = -1;
  auto source = graph.emplace([&] { source_value = 7; });
  auto sink = graph.emplace([&] { sum = std::accumulate(leaf.begin(), leaf.end(), std::int64_t{0}); });
  for (std::size_t i = 0; i < kWidth; ++i) {
    auto node = graph.emplace([&, i] { leaf[i] = source_value + static_cast<int>(i); });
    source.precede(node);
    node.precede(sink);
  }
  EXPECT_EQ(sink.num_predecessors(), kWidth);
  for (int round = 0; round < 3; ++round) {
    std::fill(leaf.begin(), leaf.end(), 0);
    graph.run(pool_).wait();
    const std::int64_t expected = 7 * static_cast<std::int64_t>(kWidth) +
                                  static_cast<std::int64_t>(kWidth) * (kWidth - 1) / 2;
    ASSERT_EQ(sum, expected);
  }
}

TEST_P(TaskGraphThreads, ManyIndependentRoots) {
  constexpr std::size_t kNodes = 5000;
  TaskGraph graph;
  std::vector<int> hits(kNodes, 0);
  for (std::size_t i = 0; i < kNodes; ++i) graph.emplace([&, i] { ++hits[i]; });
  graph.run(pool_).wait();
  graph.run(pool_).wait();
  for (std::size_t i = 0; i < kNodes; ++i) ASSERT_EQ(hits[i], 2) << i;
}

// Random DAG: edges always point from a lower to a higher rank, where ranks are a random
// permutation of the insertion order (so the build order says nothing about the
// execution order). Every node checks that all of its predecessors have finished and
// computes a value from theirs; the result is compared with a sequential evaluation.
struct RandomDag {
  std::size_t nodes = 0;
  std::vector<std::vector<std::size_t>> preds;  // by node id
};

RandomDag make_random_dag(std::uint64_t seed, std::size_t nodes, std::size_t max_preds) {
  std::mt19937_64 rng(seed);
  std::vector<std::size_t> rank(nodes);
  std::iota(rank.begin(), rank.end(), std::size_t{0});
  std::shuffle(rank.begin(), rank.end(), rng);
  // by_rank[r] = node id with rank r
  std::vector<std::size_t> by_rank(nodes);
  for (std::size_t id = 0; id < nodes; ++id) by_rank[rank[id]] = id;
  RandomDag dag;
  dag.nodes = nodes;
  dag.preds.resize(nodes);
  for (std::size_t r = 1; r < nodes; ++r) {
    const std::size_t count = std::uniform_int_distribution<std::size_t>(0, max_preds)(rng);
    for (std::size_t k = 0; k < count; ++k) {
      // Bias towards recent ranks to get long dependency chains as well as wide levels.
      const std::size_t span = std::min<std::size_t>(r, (rng() & 1) ? 8 : r);
      const std::size_t pred_rank = r - 1 - std::uniform_int_distribution<std::size_t>(0, span - 1)(rng);
      dag.preds[by_rank[r]].push_back(by_rank[pred_rank]);
    }
  }
  return dag;
}

template <class Pool>
void run_random_dag(Pool& pool, std::uint64_t seed, std::size_t nodes, std::size_t max_preds,
                    int runs) {
  SCOPED_TRACE("seed " + std::to_string(seed) + ", nodes " + std::to_string(nodes));
  const RandomDag dag = make_random_dag(seed, nodes, max_preds);

  // Sequential reference (memoized evaluation in rank order is not available here, so
  // evaluate with an explicit topological pass).
  std::vector<std::uint64_t> expected(nodes, 0);
  {
    std::vector<std::size_t> indegree(nodes, 0);
    std::vector<std::vector<std::size_t>> succs(nodes);
    for (std::size_t id = 0; id < nodes; ++id) {
      indegree[id] = dag.preds[id].size();
      for (std::size_t p : dag.preds[id]) succs[p].push_back(id);
    }
    std::vector<std::size_t> ready;
    for (std::size_t id = 0; id < nodes; ++id) {
      if (indegree[id] == 0) ready.push_back(id);
    }
    while (!ready.empty()) {
      const std::size_t id = ready.back();
      ready.pop_back();
      std::uint64_t v = id + 1;
      for (std::size_t p : dag.preds[id]) v = v * 31 + expected[p];
      expected[id] = v;
      for (std::size_t s : succs[id]) {
        if (--indegree[s] == 0) ready.push_back(s);
      }
    }
  }

  std::vector<std::uint64_t> value(nodes, 0);            // plain: checked by TSan
  auto finished = std::make_unique<std::atomic<int>[]>(nodes);  // logical order check
  std::atomic<int> current_run{0};
  std::atomic<std::size_t> violations{0};

  TaskGraph graph;
  std::vector<GraphTask> tasks;
  tasks.reserve(nodes);
  for (std::size_t id = 0; id < nodes; ++id) {
    tasks.push_back(graph.emplace([&, id] {
      const int run = current_run.load(std::memory_order_relaxed);
      std::uint64_t v = id + 1;
      for (std::size_t p : dag.preds[id]) {
        if (finished[p].load(std::memory_order_relaxed) != run) violations.fetch_add(1, std::memory_order_relaxed);
        v = v * 31 + value[p];
      }
      value[id] = v;
      if (finished[id].exchange(run, std::memory_order_relaxed) != run - 1) {
        violations.fetch_add(1, std::memory_order_relaxed);  // ran twice in one run
      }
    }));
  }
  for (std::size_t id = 0; id < nodes; ++id) {
    for (std::size_t p : dag.preds[id]) tasks[p].precede(tasks[id]);
  }
  ASSERT_FALSE(graph.has_cycle());

  for (int run = 1; run <= runs; ++run) {
    current_run.store(run, std::memory_order_relaxed);
    std::fill(value.begin(), value.end(), 0);
    graph.run(pool).wait();
    ASSERT_EQ(violations.load(), 0u) << "run " << run;
    for (std::size_t id = 0; id < nodes; ++id) {
      ASSERT_EQ(finished[id].load(std::memory_order_relaxed), run) << "node " << id;
      ASSERT_EQ(value[id], expected[id]) << "node " << id;
    }
  }
}

TEST_P(TaskGraphThreads, RandomDagsRespectDependencies) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    run_random_dag(pool_, seed * 0x9E3779B97F4A7C15ull, 50 + seed * 40, 1 + seed % 5, 3);
  }
}

TEST(TaskGraph, LargeRandomDag) {
  ThreadPool pool(8);
  run_random_dag(pool, 424242, 20000, 4, 2);
}

// A small multi-step job: read -> split into chunks -> transform each chunk in parallel
// -> reduce per chunk -> combine.
TEST_P(TaskGraphThreads, MultiStepPipeline) {
  constexpr std::size_t kChunks = 16;
  constexpr std::size_t kChunkSize = 1000;
  std::vector<int> input;
  std::vector<std::vector<int>> chunks(kChunks);
  std::vector<std::int64_t> partial(kChunks, 0);
  std::int64_t total = 0;

  TaskGraph graph;
  auto read = graph.emplace([&] {
    input.resize(kChunks * kChunkSize);
    std::iota(input.begin(), input.end(), 1);
  });
  auto combine = graph.emplace([&] { total = std::accumulate(partial.begin(), partial.end(), std::int64_t{0}); });
  for (std::size_t c = 0; c < kChunks; ++c) {
    auto split = graph.emplace([&, c] {
      chunks[c].assign(input.begin() + static_cast<std::ptrdiff_t>(c * kChunkSize),
                       input.begin() + static_cast<std::ptrdiff_t>((c + 1) * kChunkSize));
    });
    auto square = graph.emplace([&, c] {
      for (int& x : chunks[c]) x = x * 2;
    });
    auto reduce = graph.emplace([&, c] { partial[c] = std::accumulate(chunks[c].begin(), chunks[c].end(), std::int64_t{0}); });
    read.precede(split);
    split.precede(square);
    square.precede(reduce);
    reduce.precede(combine);
  }
  graph.run(pool_).wait();
  const std::int64_t n = kChunks * kChunkSize;
  EXPECT_EQ(total, n * (n + 1));  // 2 * (1 + ... + n)
}

TEST_P(TaskGraphThreads, ExceptionSkipsOnlyDependents) {
  TaskGraph graph;
  std::atomic<bool> ran_a{false}, ran_c{false}, ran_d{false}, ran_e{false}, ran_f{false};
  // a -> b(throws) -> d -> f,   a -> c -> e,   c -> f
  auto a = graph.emplace([&] { ran_a = true; });
  auto b = graph.emplace([] { throw std::runtime_error("b failed"); });
  auto c = graph.emplace([&] { ran_c = true; });
  auto d = graph.emplace([&] { ran_d = true; });
  auto e = graph.emplace([&] { ran_e = true; });
  auto f = graph.emplace([&] { ran_f = true; });
  a.precede(b, c);
  b.precede(d);
  d.precede(f);
  c.precede(e, f);
  try {
    graph.run(pool_).wait();
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "b failed");
  }
  EXPECT_TRUE(ran_a);
  EXPECT_TRUE(ran_c);
  EXPECT_TRUE(ran_e);
  EXPECT_FALSE(ran_d);
  EXPECT_FALSE(ran_f);
  EXPECT_EQ(graph.skipped_in_last_run(), 2u);
  EXPECT_FALSE(graph.is_running());
}

TEST_P(TaskGraphThreads, FirstOfSeveralExceptionsIsRethrown) {
  TaskGraph graph;
  std::atomic<int> after{0};
  auto sink = graph.emplace([&] { after.fetch_add(1); });
  for (int i = 0; i < 50; ++i) {
    auto node = graph.emplace([i] { throw std::runtime_error("node " + std::to_string(i)); });
    node.precede(sink);
  }
  for (int round = 0; round < 3; ++round) {
    EXPECT_THROW(graph.run(pool_).wait(), std::runtime_error);
    EXPECT_EQ(graph.skipped_in_last_run(), 1u);
  }
  EXPECT_EQ(after.load(), 0);
}

TEST_P(TaskGraphThreads, RerunAfterExceptionSucceeds) {
  TaskGraph graph;
  bool fail = true;
  int downstream = 0;
  auto first = graph.emplace([&] {
    if (fail) throw std::logic_error("first run fails");
  });
  auto second = graph.emplace([&] { ++downstream; });
  first.precede(second);
  EXPECT_THROW(graph.run(pool_).wait(), std::logic_error);
  EXPECT_EQ(downstream, 0);
  fail = false;
  graph.run(pool_).wait();  // must not rethrow the old exception
  EXPECT_EQ(downstream, 1);
  EXPECT_EQ(graph.skipped_in_last_run(), 0u);
}

TEST(TaskGraph, CycleIsDetectedAtRun) {
  ThreadPool pool(2);
  TaskGraph graph;
  std::atomic<int> ran{0};
  auto root = graph.emplace([&] { ++ran; });
  auto a = graph.emplace([&] { ++ran; });
  auto b = graph.emplace([&] { ++ran; });
  auto c = graph.emplace([&] { ++ran; });
  root.precede(a);
  a.precede(b);
  b.precede(c);
  EXPECT_FALSE(graph.has_cycle());
  EXPECT_NO_THROW(graph.check());
  c.precede(a);
  EXPECT_TRUE(graph.has_cycle());
  EXPECT_THROW(graph.check(), GraphCycleError);
  EXPECT_THROW((void)graph.run(pool), GraphCycleError);
  EXPECT_EQ(ran.load(), 0);  // nothing ran, not even the root
  EXPECT_FALSE(graph.is_running());

  // The graph stays usable: rebuild without the back edge.
  graph.clear();
  EXPECT_TRUE(graph.empty());
  auto x = graph.emplace([&] { ++ran; });
  auto y = graph.emplace([&] { ++ran; });
  x.precede(y);
  graph.run(pool).wait();
  EXPECT_EQ(ran.load(), 2);
}

TEST(TaskGraph, CycleWithoutRootsIsDetected) {
  ThreadPool pool(1);
  TaskGraph graph;
  auto a = graph.emplace([] {});
  auto b = graph.emplace([] {});
  a.precede(b);
  b.precede(a);
  EXPECT_THROW((void)graph.run(pool), GraphCycleError);
}

TEST(TaskGraph, SelfEdgeIsRejected) {
  TaskGraph graph;
  auto a = graph.emplace([] {});
  EXPECT_THROW(a.precede(a), GraphCycleError);
  EXPECT_EQ(a.num_successors(), 0u);
}

TEST(TaskGraph, EdgesAcrossGraphsAndEmptyHandlesAreRejected) {
  TaskGraph first, second;
  auto a = first.emplace([] {});
  auto b = second.emplace([] {});
  EXPECT_THROW(a.precede(b), std::invalid_argument);
  EXPECT_THROW(a.precede(GraphTask{}), std::invalid_argument);
  EXPECT_THROW(GraphTask{}.precede(a), std::invalid_argument);
}

TEST(TaskGraph, DuplicateEdgesAreConsistent) {
  ThreadPool pool(4);
  TaskGraph graph;
  int a = 0, b = 0;
  auto ta = graph.emplace([&] { a = 1; });
  auto tb = graph.emplace([&] { b = a + 1; });
  ta.precede(tb);
  ta.precede(tb);
  graph.run(pool).wait();
  EXPECT_EQ(b, 2);
}

TEST(TaskGraph, EmplaceSeveralReturnsHandlesInOrder) {
  ThreadPool pool(2);
  TaskGraph graph;
  std::vector<int> order;
  auto [a, b, c] = graph.emplace([&] { order.push_back(1); }, [&] { order.push_back(2); },
                                 [&] { order.push_back(3); });
  a.precede(b);
  b.precede(c);
  graph.run(pool).wait();
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(TaskGraph, MoveOnlyCallableAndReturnValueIgnored) {
  ThreadPool pool(2);
  TaskGraph graph;
  auto value = std::make_unique<int>(41);
  int seen = 0;
  graph.emplace([v = std::move(value), &seen] {
    seen = *v + 1;
    return seen;  // ignored
  });
  graph.run(pool).wait();
  EXPECT_EQ(seen, 42);
}

TEST(TaskGraph, RerunManyTimes) {
  ThreadPool pool(4);
  TaskGraph graph;
  constexpr int kRuns = 200;
  std::vector<int> counts(10, 0);
  std::vector<GraphTask> nodes;
  for (std::size_t i = 0; i < counts.size(); ++i) nodes.push_back(graph.emplace([&, i] { ++counts[i]; }));
  for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
    nodes[i].precede(nodes[i + 1]);
    if (i + 3 < nodes.size()) nodes[i].precede(nodes[i + 3]);
  }
  for (int run = 0; run < kRuns; ++run) graph.run(pool).wait();
  for (int count : counts) EXPECT_EQ(count, kRuns);
}

TEST(TaskGraph, GraphCanGrowBetweenRuns) {
  ThreadPool pool(2);
  TaskGraph graph;
  std::vector<int> order;
  auto a = graph.emplace([&] { order.push_back(1); });
  graph.run(pool).wait();
  auto b = graph.emplace([&] { order.push_back(2); });
  b.succeed(a);
  graph.run(pool).wait();
  EXPECT_EQ(order, (std::vector<int>{1, 1, 2}));
}

TEST(TaskGraph, BuildingOrRunningWhileRunningIsRejected) {
  ThreadPool pool(2);
  TaskGraph graph;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::promise<void> started;
  auto first = graph.emplace([&] {
    started.set_value();
    released.wait();
  });
  auto other = graph.emplace([] {});
  auto run = graph.run(pool);
  started.get_future().wait();
  EXPECT_TRUE(graph.is_running());
  EXPECT_FALSE(run.is_ready());
  EXPECT_THROW((void)graph.run(pool), std::logic_error);
  EXPECT_THROW(graph.emplace([] {}), std::logic_error);
  EXPECT_THROW(graph.clear(), std::logic_error);
  EXPECT_THROW(first.precede(other), std::logic_error);
  release.set_value();
  run.wait();
  EXPECT_FALSE(graph.is_running());
  EXPECT_FALSE(run.valid());
}

TEST(TaskGraph, HandleDestructorWaitsForRun) {
  ThreadPool pool(2);
  std::atomic<int> done{0};
  {
    TaskGraph graph;
    auto a = graph.emplace([&] { done.fetch_add(1); });
    auto b = graph.emplace([&] { done.fetch_add(1); });
    a.precede(b);
    auto run = graph.run(pool);
    // The handle is destroyed before the graph (reverse declaration order) and waits.
  }
  EXPECT_EQ(done.load(), 2);
}

TEST(TaskGraph, HandleDestructorDiscardsException) {
  ThreadPool pool(2);
  TaskGraph graph;
  graph.emplace([] { throw std::runtime_error("unobserved"); });
  { auto run = graph.run(pool); }
  EXPECT_FALSE(graph.is_running());
  EXPECT_NO_THROW(graph.check());
}

TEST(TaskGraph, MovedHandleWaitsOnce) {
  ThreadPool pool(2);
  TaskGraph graph;
  int runs = 0;
  graph.emplace([&] { ++runs; });
  auto first = graph.run(pool);
  stealpool::RunHandle second = std::move(first);
  EXPECT_FALSE(first.valid());
  second.wait();
  first.wait();  // no-op on an empty handle
  EXPECT_EQ(runs, 1);
}

template <class Pool>
void run_nested_graphs(std::size_t threads) {
  Pool pool(threads);
  TaskGraph outer;
  std::atomic<int> leaves{0};
  std::int64_t result = 0;
  std::vector<std::int64_t> branch(4, 0);
  auto join = outer.emplace([&] { result = branch[0] + branch[1] + branch[2] + branch[3]; });
  for (std::size_t i = 0; i < branch.size(); ++i) {
    auto node = outer.emplace([&, i] {
      if (i % 2 == 0) {
        stealpool::TaskGroup group(pool);
        for (int k = 0; k < 10; ++k) group.spawn([&] { leaves.fetch_add(1); });
        group.wait();
      } else {
        TaskGraph inner;
        auto x = inner.emplace([&] { leaves.fetch_add(1); });
        auto y = inner.emplace([&] { leaves.fetch_add(1); });
        x.precede(y);
        inner.run(pool).wait();
      }
      branch[i] = static_cast<std::int64_t>(i) + 1;
    });
    node.precede(join);
  }
  outer.run(pool).wait();
  EXPECT_EQ(result, 10) << threads << " workers";
  EXPECT_EQ(leaves.load(), 2 * 10 + 2 * 2) << threads << " workers";
}

// One worker: every wait inside a node must help, or this deadlocks.
TEST(TaskGraph, NodesUseTaskGroupsAndNestedGraphs) {
  run_nested_graphs<ThreadPool>(1);
  run_nested_graphs<ThreadPool>(4);
}

// The graph is written against the executor interface only, so it runs unchanged on the
// shared-queue baseline pool.
TEST(TaskGraph, SharedQueuePoolRandomDags) {
  for (std::size_t threads : {1u, 4u}) {
    stealpool::SharedQueuePool pool(threads);
    for (std::uint64_t seed = 1; seed <= 6; ++seed) run_random_dag(pool, seed * 7919, 200 + seed * 50, 3, 2);
  }
}

TEST(TaskGraph, SharedQueuePoolNestedGraphs) {
  run_nested_graphs<stealpool::SharedQueuePool>(1);
  run_nested_graphs<stealpool::SharedQueuePool>(4);
}

TEST(TaskGraph, SharedQueuePoolExceptionSkipsDependents) {
  stealpool::SharedQueuePool pool(3);
  TaskGraph graph;
  std::atomic<bool> ran_sibling{false}, ran_dependent{false};
  auto root = graph.emplace([] {});
  auto failing = graph.emplace([] { throw std::runtime_error("boom"); });
  auto sibling = graph.emplace([&] { ran_sibling = true; });
  auto dependent = graph.emplace([&] { ran_dependent = true; });
  root.precede(failing, sibling);
  failing.precede(dependent);
  EXPECT_THROW(graph.run(pool).wait(), std::runtime_error);
  EXPECT_TRUE(ran_sibling);
  EXPECT_FALSE(ran_dependent);
}

TEST(TaskGraph, ManyConcurrentGraphsFromExternalThreads) {
  ThreadPool pool(4);
  constexpr int kThreads = 6;
  constexpr int kRuns = 25;
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      TaskGraph graph;
      std::vector<int> value(64, 0);
      std::vector<GraphTask> nodes;
      for (std::size_t i = 0; i < value.size(); ++i) {
        nodes.push_back(graph.emplace([&, i] {
          // Binary-heap shaped dependencies: node i depends on its parent.
          value[i] = (i == 0 ? t : value[(i - 1) / 2]) + 1;
        }));
        if (i > 0) nodes[(i - 1) / 2].precede(nodes[i]);
      }
      for (int run = 0; run < kRuns; ++run) {
        std::fill(value.begin(), value.end(), 0);
        graph.run(pool).wait();
        for (std::size_t i = 0; i < value.size(); ++i) {
          int depth = 0;
          for (std::size_t j = i; j > 0; j = (j - 1) / 2) ++depth;
          if (value[i] != t + depth + 1) failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST(TaskGraph, ManyConcurrentGraphsInsideThePool) {
  ThreadPool pool(4);
  constexpr std::size_t kGraphs = 32;
  std::vector<std::int64_t> sums(kGraphs, 0);
  stealpool::TaskGroup group(pool);
  for (std::size_t g = 0; g < kGraphs; ++g) {
    group.spawn([&, g] {
      TaskGraph graph;
      std::vector<std::int64_t> values(20, 0);
      auto sum = graph.emplace([&] { sums[g] = std::accumulate(values.begin(), values.end(), std::int64_t{0}); });
      for (std::size_t i = 0; i < values.size(); ++i) {
        graph.emplace([&, i] { values[i] = static_cast<std::int64_t>(g * i); }).precede(sum);
      }
      for (int run = 0; run < 3; ++run) graph.run(pool).wait();
    });
  }
  group.wait();
  for (std::size_t g = 0; g < kGraphs; ++g) EXPECT_EQ(sums[g], static_cast<std::int64_t>(g * 190)) << g;
}

TEST(TaskGraph, RunsOnAnyExecutorReference) {
  ThreadPool pool(2);
  stealpool::Executor& executor = pool;
  TaskGraph graph;
  int a = 0, b = 0;
  graph.emplace([&] { a = 1; }).precede(graph.emplace([&] { b = a + 1; }));
  graph.run(executor).wait();
  EXPECT_EQ(b, 2);
}


// Two threads take turns running one graph whose node always throws. Each thread starts its
// run as soon as the other one's wait() has released the graph (is_running() turns false),
// so a new run starts while the previous wait() may still be finishing. That wait() must
// still rethrow its own exception: it has to take the error out before it lets the next run
// reset the run state. The window is a few instructions wide, so this relies on repetition:
// with the error read after the release of the graph, 20000 rounds crashed every run
// (double free of the exception under ASan, SIGSEGV/SIGABRT in release). TSan does not flag
// that bug because the exception_ptr assignments live in the uninstrumented C++ runtime.
TEST(TaskGraph, NextRunMayStartWhileTheLastWaitRethrows) {
  ThreadPool pool(2);
  TaskGraph graph;
  auto failing = graph.emplace([] { throw std::runtime_error("always"); });
  failing.precede(graph.emplace([] {}));
  constexpr int kRounds = 20000;
  std::atomic<int> started{0};  // runs started so far; round r belongs to thread r % 2
  std::atomic<int> rethrown{0};
  std::atomic<int> wrong{0};
  auto take_turns = [&](int first_round) {
    for (int round = first_round; round < kRounds; round += 2) {
      while (started.load(std::memory_order_acquire) != round) std::this_thread::yield();
      while (graph.is_running()) std::this_thread::yield();
      auto run = graph.run(pool);  // only this thread may call run() in this round
      started.store(round + 1, std::memory_order_release);
      try {
        run.wait();
        wrong.fetch_add(1);
      } catch (const std::runtime_error&) {
        rethrown.fetch_add(1);
      } catch (...) {
        wrong.fetch_add(1);
      }
    }
  };
  std::thread other(take_turns, 1);
  take_turns(0);
  other.join();
  EXPECT_EQ(rethrown.load(), kRounds);
  EXPECT_EQ(wrong.load(), 0);
  EXPECT_FALSE(graph.is_running());
}

// Forwards to a ThreadPool, except that the n-th schedule() call made by the thread that
// created it throws once, the way a pool queue that cannot grow would. TaskGraph::run()
// schedules the roots on the calling thread in the order they were added, so this picks
// one root; schedule() calls from the pool's workers are never counted.
class FailingScheduleExecutor final : public stealpool::Executor {
 public:
  explicit FailingScheduleExecutor(std::size_t threads) : pool_(threads) {}

  void fail_schedule_call(int n) { countdown_ = n; }

  std::size_t concurrency() const noexcept override { return pool_.concurrency(); }
  void schedule(stealpool::detail::Task* task) override {
    if (std::this_thread::get_id() == owner_ && countdown_ > 0 && --countdown_ == 0) throw std::bad_alloc();
    pool_.schedule(task);
  }
  void wait_for(stealpool::detail::CompletionEvent& event) override { pool_.wait_for(event); }

 private:
  ThreadPool pool_;
  const std::thread::id owner_ = std::this_thread::get_id();
  int countdown_ = 0;  // only touched by owner_
};

// Three independent chains. Scheduling the second root fails, after the first root may
// already be running: the run must still settle (no node left pointing into a graph that
// nobody waits for), the failed root and every root after it count as failed, their
// dependents are skipped, and wait() rethrows the scheduling error.
TEST(TaskGraph, RootSchedulingFailureSettlesTheRun) {
  FailingScheduleExecutor executor(2);
  TaskGraph graph;
  std::atomic<int> ran[6] = {};
  std::vector<GraphTask> nodes;
  for (int i = 0; i < 6; ++i) nodes.push_back(graph.emplace([&ran, i] { ran[i].fetch_add(1); }));
  nodes[0].precede(nodes[3]);
  nodes[1].precede(nodes[4]);
  nodes[2].precede(nodes[5]);

  executor.fail_schedule_call(2);  // the second root, nodes[1]
  auto run = graph.run(executor);
  EXPECT_THROW(run.wait(), std::bad_alloc);
  EXPECT_FALSE(graph.is_running());
  EXPECT_EQ(ran[0].load(), 1);
  EXPECT_EQ(ran[3].load(), 1);
  for (int i : {1, 2, 4, 5}) EXPECT_EQ(ran[i].load(), 0) << "node " << i;
  EXPECT_EQ(graph.skipped_in_last_run(), 2u);

  // The graph is intact: the next run executes everything.
  graph.run(executor).wait();
  for (int i = 0; i < 6; ++i) EXPECT_EQ(ran[i].load(), i == 0 || i == 3 ? 2 : 1) << "node " << i;
}

TEST(TaskGraph, FirstRootSchedulingFailureRunsNothing) {
  FailingScheduleExecutor executor(1);
  TaskGraph graph;
  std::atomic<int> ran{0};
  auto a = graph.emplace([&] { ran.fetch_add(1); });
  auto b = graph.emplace([&] { ran.fetch_add(1); });
  a.precede(b);
  executor.fail_schedule_call(1);  // the only root, a
  EXPECT_THROW(graph.run(executor).wait(), std::bad_alloc);
  EXPECT_FALSE(graph.is_running());
  EXPECT_EQ(ran.load(), 0);
  EXPECT_EQ(graph.skipped_in_last_run(), 1u);
  graph.run(executor).wait();
  EXPECT_EQ(ran.load(), 2);
}

}  // namespace
