// Task dependencies: a reusable directed acyclic graph of tasks.
//
//   TaskGraph graph;
//   auto load  = graph.emplace([&] { load_input(); });
//   auto parse = graph.emplace([&] { parse_input(); });
//   auto stats = graph.emplace([&] { compute_stats(); });
//   auto index = graph.emplace([&] { build_index(); });
//   auto save  = graph.emplace([&] { write_output(); });
//   load.precede(parse);
//   parse.precede(stats, index);   // stats and index may run in parallel
//   save.succeed(stats, index);    // save waits for both
//   graph.run(pool).wait();        // rethrows the first exception, if any
//
// A node runs only after every one of its predecessors has finished. The graph is built
// on the public executor interface only (Executor::schedule / Executor::wait_for), so it
// runs unchanged on any pool: ThreadPool, SharedQueuePool, or another Executor.
//
// Execution: every node carries an atomic count of unfinished predecessors, reset at the
// start of each run. A finishing node decrements the count of each successor; the one
// that brings a count to zero schedules that successor from the thread it is running on
// (on ThreadPool that is the worker's own deque, so the successor is likely to run on
// the same core while its inputs are still in cache, and idle workers steal the rest).
// The node objects themselves are the scheduler's task objects, so running a graph does
// not allocate.
//
// Exceptions: when a node throws, the first exception (in completion order) is captured
// and every node that transitively depends on a failed node is skipped (its callable is
// not invoked). Nodes that do not depend on the failure still run. RunHandle::wait()
// rethrows the captured exception after the whole run has settled.
//
// Cycles: run() checks the graph (Kahn's algorithm) whenever it changed since the last
// check and throws GraphCycleError without running anything. check() and has_cycle()
// expose the same test while building.
//
// Threading rules:
//   * Building (emplace / precede / succeed) is single-threaded and not allowed while the
//     graph is running (throws std::logic_error).
//   * One run at a time per graph; run() on a running graph throws std::logic_error. The
//     graph can be run again after the previous run was waited for, any number of times.
//     Different graphs may run concurrently on the same pool.
//   * A node may itself spawn TaskGroups, submit tasks or run (and wait for) another
//     graph on the same pool; waits on a worker thread help, so plain nesting does not
//     deadlock. The helping-wait rule of executor.hpp (TaskHandle) still applies: a node
//     must not block on another task's continuation and must not hold a lock across a
//     wait, or the pool can deadlock even with idle workers.
//   * The graph must outlive its runs: RunHandle's destructor waits for the run.
//   * Out of memory while scheduling a ready successor terminates the program (tasks are
//     noexcept); the scheduler queues themselves are the only allocation on that path.
//     If run() itself fails to schedule a root (e.g. std::bad_alloc from the pool's
//     queue), that root counts as a failed node: its dependents are skipped, run() still
//     returns a handle, and wait() rethrows the scheduling exception once the run settled.
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "stealpool/detail/task.hpp"
#include "stealpool/executor.hpp"

namespace stealpool {

// Thrown by TaskGraph::run() and TaskGraph::check() when the dependencies form a cycle.
class GraphCycleError : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

class TaskGraph;
class GraphTask;

namespace detail {

// One vertex of a TaskGraph. It derives from Task so the executor can queue the node
// itself; the node is not freed after it runs (the graph owns it), which is what makes
// a graph re-runnable without allocation.
class GraphNode : public Task {
 public:
  GraphNode(const GraphNode&) = delete;
  GraphNode& operator=(const GraphNode&) = delete;
  virtual ~GraphNode() = default;

 protected:
  explicit GraphNode(TaskGraph& graph) noexcept : Task(&execute_node), graph_(&graph) {}

  virtual void invoke() = 0;

 private:
  friend class stealpool::TaskGraph;
  friend class stealpool::GraphTask;

  static void execute_node(Task* base) noexcept;

  TaskGraph* graph_;
  std::size_t index_ = 0;  // position in the graph's node list
  std::vector<GraphNode*> successors_;
  std::size_t num_predecessors_ = 0;

  // Per-run state, reset by TaskGraph::start() before any node of the run is scheduled.
  std::atomic<std::size_t> pending_{0};  // unfinished predecessors in the current run
  std::atomic<bool> skip_{false};        // some predecessor failed or was skipped
};

template <class F>
class CallableNode final : public GraphNode {
 public:
  template <class G>
  CallableNode(TaskGraph& graph, G&& fn) : GraphNode(graph), fn_(std::forward<G>(fn)) {}

 private:
  void invoke() override { std::invoke(fn_); }

  F fn_;
};

}  // namespace detail

// Lightweight, copyable reference to a node of a TaskGraph. Valid as long as the graph.
class GraphTask {
 public:
  GraphTask() noexcept = default;

  bool valid() const noexcept { return node_ != nullptr; }

  // Adds the edges this -> each of `others`: the others run only after this node finished.
  template <class... Others>
    requires(std::same_as<Others, GraphTask> && ...)
  GraphTask& precede(Others... others) {
    (add_edge(*this, others), ...);
    return *this;
  }

  // Adds the edges each of `others` -> this.
  template <class... Others>
    requires(std::same_as<Others, GraphTask> && ...)
  GraphTask& succeed(Others... others) {
    (add_edge(others, *this), ...);
    return *this;
  }

  std::size_t num_successors() const noexcept { return node_->successors_.size(); }
  std::size_t num_predecessors() const noexcept { return node_->num_predecessors_; }

  friend bool operator==(const GraphTask&, const GraphTask&) noexcept = default;

 private:
  friend class TaskGraph;
  explicit GraphTask(detail::GraphNode* node) noexcept : node_(node) {}

  static void add_edge(GraphTask from, GraphTask to);

  detail::GraphNode* node_ = nullptr;
};

// The result of TaskGraph::run(): waits for the run and reports its exception. Move-only;
// destroying a handle that was not waited for waits (and discards the exception), so the
// graph can never be modified or destroyed under a running node.
class [[nodiscard]] RunHandle {
 public:
  RunHandle() noexcept = default;
  RunHandle(RunHandle&& other) noexcept : graph_(std::exchange(other.graph_, nullptr)) {}
  RunHandle& operator=(RunHandle&& other) noexcept {
    if (this != &other) {
      finish_quietly();
      graph_ = std::exchange(other.graph_, nullptr);
    }
    return *this;
  }
  ~RunHandle() { finish_quietly(); }

  bool valid() const noexcept { return graph_ != nullptr; }

  // True once every node of the run has finished or been skipped.
  bool is_ready() const noexcept;

  // Waits for the run (helping on a worker thread, blocking elsewhere), then rethrows
  // the first exception thrown by a node. Invalidates the handle; afterwards the graph
  // may be modified or run again.
  void wait();

 private:
  friend class TaskGraph;
  explicit RunHandle(TaskGraph* graph) noexcept : graph_(graph) {}

  void finish_quietly() noexcept {
    try {
      wait();
    } catch (...) {
      // Intentionally discarded; see the class comment.
    }
  }

  TaskGraph* graph_ = nullptr;
};

class TaskGraph {
 public:
  TaskGraph() = default;
  TaskGraph(const TaskGraph&) = delete;
  TaskGraph& operator=(const TaskGraph&) = delete;
  ~TaskGraph() = default;  // runs are waited for by their RunHandle

  // Adds a node that will invoke `fn()` (a callable taking no arguments; its result, if
  // any, is ignored). Move-only callables are fine.
  template <class F>
  GraphTask emplace(F&& fn) {
    static_assert(std::is_invocable_v<std::decay_t<F>&>, "graph tasks take no arguments");
    ensure_idle("TaskGraph::emplace");
    auto node = std::make_unique<detail::CallableNode<std::decay_t<F>>>(*this, std::forward<F>(fn));
    node->index_ = nodes_.size();
    GraphTask handle(node.get());
    nodes_.push_back(std::move(node));
    validated_ = false;
    return handle;
  }

  // Adds one node per callable and returns their handles, in order.
  template <class... Fs>
    requires(sizeof...(Fs) > 1)
  auto emplace(Fs&&... fns) {
    return std::make_tuple(emplace(std::forward<Fs>(fns))...);
  }

  std::size_t size() const noexcept { return nodes_.size(); }
  bool empty() const noexcept { return nodes_.empty(); }

  // Removes every node (invalidating all GraphTask handles). Not allowed while running.
  void clear() {
    ensure_idle("TaskGraph::clear");
    nodes_.clear();
    roots_.clear();
    validated_ = true;
  }

  bool has_cycle() const;

  // Throws GraphCycleError if the dependencies contain a cycle.
  void check() const {
    if (has_cycle()) throw GraphCycleError("stealpool::TaskGraph: dependency cycle");
  }

  // Starts a run on `pool` and returns a handle to wait for it. Throws GraphCycleError
  // (nothing is run) if the graph has a cycle, std::logic_error if it is already running.
  // `Pool` is any executor (ThreadPool, SharedQueuePool, ...); nodes are scheduled on it.
  template <class Pool>
    requires std::derived_from<Pool, Executor>
  RunHandle run(Pool& pool) {
    return start(static_cast<Executor&>(pool));
  }

  // Convenience: run(pool).wait().
  template <class Pool>
    requires std::derived_from<Pool, Executor>
  void run_and_wait(Pool& pool) {
    run(pool).wait();
  }

  bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }

  // Nodes of the last finished run that were skipped because a predecessor failed.
  std::size_t skipped_in_last_run() const noexcept { return skipped_.load(std::memory_order_relaxed); }

 private:
  friend class GraphTask;
  friend class RunHandle;
  friend class detail::GraphNode;

  void ensure_idle(const char* what) const {
    if (running_.load(std::memory_order_acquire)) {
      throw std::logic_error(std::string(what) + " while the graph is running");
    }
  }

  // Validates the topology (once per change), resets the per-run state and schedules
  // the root nodes.
  RunHandle start(Executor& executor);

  void finish_wait();
  void node_finished() noexcept;
  // Propagates a node's outcome to its successors (scheduling those that became ready)
  // and counts it as finished. The node must not be touched afterwards.
  static void complete_node(detail::GraphNode& node, bool failed) noexcept;
  void record_exception(std::exception_ptr error) noexcept {
    // First thread to flip the flag owns error_; the write reaches the waiter through
    // this node's release decrement of remaining_.
    if (!has_error_.exchange(true, std::memory_order_relaxed)) error_ = std::move(error);
  }

  std::vector<std::unique_ptr<detail::GraphNode>> nodes_;
  std::vector<detail::GraphNode*> roots_;  // nodes without predecessors; valid if validated_
  bool validated_ = true;                  // an empty graph is trivially acyclic

  // Run state. running_ is set by start() and cleared by the RunHandle's wait.
  std::atomic<bool> running_{false};
  Executor* executor_ = nullptr;
  std::atomic<std::size_t> remaining_{0};  // nodes of the current run not yet finished
  detail::CompletionEvent done_;
  std::atomic<bool> has_error_{false};
  std::exception_ptr error_;
  std::atomic<std::size_t> skipped_{0};
};

// ---------------------------------------------------------------------------------------
// Implementation

inline void GraphTask::add_edge(GraphTask from, GraphTask to) {
  if (!from.valid() || !to.valid()) throw std::invalid_argument("stealpool::GraphTask: empty handle");
  TaskGraph& graph = *from.node_->graph_;
  if (to.node_->graph_ != &graph) {
    throw std::invalid_argument("stealpool::GraphTask: nodes belong to different graphs");
  }
  if (from == to) throw GraphCycleError("stealpool::TaskGraph: a task cannot precede itself");
  graph.ensure_idle("GraphTask::precede");
  from.node_->successors_.push_back(to.node_);
  ++to.node_->num_predecessors_;
  graph.validated_ = false;
}

inline bool TaskGraph::has_cycle() const {
  // Kahn's algorithm: repeatedly remove nodes whose predecessors are all removed. A cycle
  // is exactly what is left over. Duplicate edges are counted on both sides, so they are
  // consistent with the run-time counters.
  std::vector<detail::GraphNode*> ready;
  std::vector<std::size_t> indegree(nodes_.size());
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    indegree[i] = nodes_[i]->num_predecessors_;
    if (indegree[i] == 0) ready.push_back(nodes_[i].get());
  }
  std::size_t removed = 0;
  while (!ready.empty()) {
    detail::GraphNode* node = ready.back();
    ready.pop_back();
    ++removed;
    for (detail::GraphNode* successor : node->successors_) {
      if (--indegree[successor->index_] == 0) ready.push_back(successor);
    }
  }
  return removed != nodes_.size();
}

inline RunHandle TaskGraph::start(Executor& executor) {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    throw std::logic_error("stealpool::TaskGraph::run while the graph is already running");
  }
  if (!validated_) {
    // Nothing is scheduled yet, so any failure here (a cycle, or bad_alloc from the
    // check or the roots list) simply undoes the claim on the graph. validated_ stays
    // false, so a partially rebuilt roots_ is never used.
    try {
      if (has_cycle()) throw GraphCycleError("stealpool::TaskGraph: dependency cycle");
      roots_.clear();
      for (const auto& node : nodes_) {
        if (node->num_predecessors_ == 0) roots_.push_back(node.get());
      }
    } catch (...) {
      running_.store(false, std::memory_order_release);
      throw;
    }
    validated_ = true;
  }

  executor_ = &executor;
  has_error_.store(false, std::memory_order_relaxed);
  error_ = nullptr;
  skipped_.store(0, std::memory_order_relaxed);
  if (nodes_.empty()) {
    // Nothing will ever set done_, so complete the run here.
    remaining_.store(0, std::memory_order_relaxed);
    (void)done_.set();
    return RunHandle(this);
  }
  // relaxed is enough for the resets: they are sequenced before the schedule() calls
  // below, and every node runs after (happens-after) one of those calls.
  for (const auto& node : nodes_) {
    node->pending_.store(node->num_predecessors_, std::memory_order_relaxed);
    node->skip_.store(false, std::memory_order_relaxed);
  }
  remaining_.store(nodes_.size(), std::memory_order_relaxed);
  // The roots may start (and the whole graph may even finish) while this loop is still
  // running, which is fine: roots_ is only read here and the graph is not destroyed
  // before the RunHandle returned below has waited.
  //
  // Once one root is queued the run has started and must be settled, so a schedule()
  // that throws does not escape: the root that was not queued becomes a failed node
  // (its exception is the run's error, its dependents are skipped) and the roots after
  // it are completed the same way here. The caller gets a handle as usual and wait()
  // rethrows. Without this, running_ would stay set and queued roots would point into a
  // graph that nobody waits for.
  std::size_t scheduled = 0;
  try {
    for (; scheduled < roots_.size(); ++scheduled) executor.schedule(roots_[scheduled]);
  } catch (...) {
    record_exception(std::current_exception());
    for (std::size_t i = scheduled; i < roots_.size(); ++i) complete_node(*roots_[i], /*failed=*/true);
  }
  return RunHandle(this);
}

inline void TaskGraph::node_finished() noexcept {
  // acq_rel: release publishes this node's effects (and error_); acquire on the last
  // decrement gathers every other node's effects before done_ is set, so the waiter
  // sees all of them.
  if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    // Last access to the graph by this run. The Parker belongs to the waiting thread.
    if (detail::Parker* waiter = done_.set()) waiter->unpark();
  }
}

inline void TaskGraph::finish_wait() {
  if (!done_.is_set()) executor_->wait_for(done_);
  // Every node of the run has made its final access (the last one set done_). Take the
  // error out before clearing running_: that store hands the graph to the next run(),
  // which resets has_error_ and error_ from another thread.
  std::exception_ptr error;
  if (has_error_.load(std::memory_order_relaxed)) {
    has_error_.store(false, std::memory_order_relaxed);
    error = std::exchange(error_, nullptr);
  }
  done_.reset();
  // Must be the last access to the run state.
  running_.store(false, std::memory_order_release);
  if (error) std::rethrow_exception(std::move(error));
}

inline void detail::GraphNode::execute_node(Task* base) noexcept {
  auto* self = static_cast<GraphNode*>(base);
  TaskGraph& graph = *self->graph_;
  // skip_ was written (relaxed) by a predecessor before its release decrement of
  // pending_; this node was scheduled by the acquiring last decrement, so it is visible.
  bool failed = self->skip_.load(std::memory_order_relaxed);
  if (failed) {
    graph.skipped_.fetch_add(1, std::memory_order_relaxed);
  } else {
    try {
      self->invoke();
    } catch (...) {
      graph.record_exception(std::current_exception());
      failed = true;
    }
  }
  TaskGraph::complete_node(*self, failed);
}

inline void TaskGraph::complete_node(detail::GraphNode& node, bool failed) noexcept {
  TaskGraph& graph = *node.graph_;
  for (detail::GraphNode* successor : node.successors_) {
    if (failed) successor->skip_.store(true, std::memory_order_relaxed);
    // acq_rel: release hands this node's effects to the successor; the acquire on the
    // last decrement makes all predecessors' effects visible to whoever schedules it,
    // and schedule() publishes them to the thread that runs it.
    if (successor->pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      graph.executor_->schedule(successor);
    }
  }
  // Must come last: when it completes the run, the graph may be modified or destroyed.
  graph.node_finished();
}

inline bool RunHandle::is_ready() const noexcept { return graph_->done_.is_set(); }

inline void RunHandle::wait() {
  if (graph_ == nullptr) return;
  std::exchange(graph_, nullptr)->finish_wait();
}

}  // namespace stealpool
