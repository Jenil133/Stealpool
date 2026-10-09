# Stealpool design

Stealpool is a work-stealing task scheduler for C++20 that uses only `std::thread` and
`std::atomic`. This document covers the deque, the scheduling loop, parking, helping
waits, shutdown, the task-graph executor, the shared-queue baseline pool and the stress
tests. Each design decision comes with the reason for it.

## Layout

| File | Contents |
| --- | --- |
| `include/stealpool/deque.hpp` | `WorkStealingDeque<T>`, a Chase-Lev deque |
| `include/stealpool/detail/task.hpp` | `Task` (intrusive task header), `Parker`, `CompletionEvent` |
| `include/stealpool/executor.hpp` | `Executor` interface, `submit()`, `TaskHandle<R>` |
| `include/stealpool/task_group.hpp` | `TaskGroup` (fork-join scope) |
| `include/stealpool/parallel_for.hpp` | `parallel_for`, `parallel_for_blocked` |
| `include/stealpool/thread_pool.hpp`, `src/thread_pool.cpp` | `ThreadPool`, the work-stealing executor |
| `include/stealpool/task_graph.hpp` | `TaskGraph`, `GraphTask`, `RunHandle`, `GraphCycleError` (task dependencies, header-only) |
| `include/stealpool/shared_queue_pool.hpp`, `src/shared_queue_pool.cpp` | `SharedQueuePool`, the single-queue baseline executor |
| `include/stealpool/stealpool.hpp` | umbrella header: everything above |
| `examples/` | recursive Fibonacci, parallel quicksort, unbalanced tree reduction, multi-step graph pipeline |
| `bench/` | Google Benchmark scaling suite (`scaling_bench.cpp`) and its pool-generic workloads (`workloads.hpp`) |
| `scripts/` | `run_benchmarks.sh` (full benchmark run), `bench_report.py` (JSON to speedup tables) |
| `tests/` | GoogleTest suites (`unit` label), randomized `stress` suites, benchmark smoke tests (`bench` label) |

## Public API

```cpp
stealpool::ThreadPool pool(8);                       // or ThreadPool() = hardware_concurrency

auto h = pool.submit([] { return 42; });             // TaskHandle<int>, move-only
int v = h.get();                                     // waits; rethrows the task's exception

stealpool::TaskGroup g(pool);                        // fork-join scope
g.spawn([&] { left(); });
right();
g.wait();                                            // helps while waiting; rethrows first exception

stealpool::parallel_for(pool, 0, n, [&](std::size_t i) { out[i] = f(i); });
stealpool::parallel_for_blocked(pool, 0, n, grain, [&](std::size_t lo, std::size_t hi) { ... });

stealpool::TaskGraph graph;                          // dependencies
auto [load, parse, report] = graph.emplace(load_fn, parse_fn, report_fn);
load.precede(parse);
parse.precede(report);
graph.run(pool).wait();                              // re-runnable; rethrows the first exception

stealpool::SharedQueuePool baseline(8);              // same API, one shared queue
```

The bodies of `parallel_for` and `parallel_for_blocked` are called concurrently from several
threads through a `const` reference; a `mutable` lambda is rejected at compile time.

`TaskHandle`, `TaskGroup` and the algorithms are written against the abstract
`Executor`, which has just two scheduler-specific operations:

* `schedule(detail::Task*)`: make a task runnable;
* `wait_for(detail::CompletionEvent&)`: return once the event is set. On one of the
  executor's own workers this helps (runs other tasks); on any other thread it blocks.

Another pool (for example a shared-queue baseline) only has to implement these two
functions to run the same `TaskGroup`, `submit` and `parallel_for` code. That keeps a
benchmark comparison fair, because the only thing that changes is the queueing strategy.

## Tasks

A task is one heap object: a `detail::Task` header (a single function pointer) followed by
the callable and, for `submit()`, the result slot. The queues move a raw `Task*`. Compared
with `std::function` there is no second allocation for large captures, no type-erasure
vtable, and no copy of the callable when it moves between queues. The scheduler calls
`execute()` exactly once. The concrete task type runs the callable, publishes completion
and frees itself.

* `TaskGroup::spawn` allocates a `GroupTask<F>` (callable plus a reference to the group).
  After the callable runs, the task frees itself and *then* signals the group. Captures
  are therefore destroyed before `wait()` can return.
* `submit` allocates a `SubmittedTask<R, F>`, which is also the shared state between the
  task and its `TaskHandle`. It is reference counted with two references: the executing
  side and the handle. A dropped handle detaches the task, which still runs.

## WorkStealingDeque (Chase-Lev)

The owner calls `push` and `pop` at the bottom (LIFO), and thieves call `steal` at the top
(FIFO, CAS on `top`). The deque uses a power-of-two circular buffer that doubles when it
is full. A thief can still be reading an old buffer, so retired buffers stay alive until
the deque is destroyed. Their total size is less than the size of the current buffer
(geometric series). Elements can be any trivially copyable `T` whose `std::atomic<T>` is
lock-free. The pool stores `Task*`.

### Memory ordering

The published algorithm for weak memory models (Le et al., PPoPP 2013) relies on
standalone `seq_cst` fences. ThreadSanitizer does not model `atomic_thread_fence`, so a
fence-based deque cannot be checked with TSan. Stealpool therefore puts the ordering on
the `top`/`bottom` operations themselves:

| Operation | Ordering | Why |
| --- | --- | --- |
| `push`: load `top` | acquire | pairs with a thief's successful CAS: a slot whose element was stolen can be reused safely |
| `push`: store slot | relaxed | published by the following `bottom` store |
| `push`: store `bottom` | seq_cst | release publishes the slot; seq_cst also makes it the store half of the parking handshake (below) |
| `pop`: store `bottom = b-1` | seq_cst | store→load ordering with the following `top` load (Dekker pattern against `steal`) |
| `pop`: load `top` | seq_cst | other half of the pattern above |
| `pop`: CAS `top` on the last element | seq_cst | owner and thieves race for the last element through the same CAS: exactly one wins |
| `pop`: restore `bottom` | release | every `bottom` store stays a release, so a thief reading *any* `bottom` value also sees the slot writes before it (C++20 release sequences no longer include plain stores by the same thread) |
| `steal`: load `top`, then `bottom` | seq_cst, seq_cst | mirror of `pop`; the `bottom` load also acquires the slot write |
| `steal`: load buffer pointer | acquire | pairs with the release store in `grow()`, so copied slots are visible |
| `steal`: CAS `top` | seq_cst | claims the element; on failure the value read is discarded |
| slots | relaxed atomics | a thief may read a slot the owner is overwriting after wrap-around; its CAS then fails. Atomic slots make this race well-defined |

Why seq_cst on both sides of `pop`/`steal`: suppose a thief has advanced `top` to `t+1 = b`,
and a second thief reads `top = t+1` and the old `bottom = b+1`. If the owner then reads a
stale `top = t`, it takes element `b` without a CAS while the second thief also claims `b`.
With all four accesses seq_cst, the second thief's `bottom` load comes before the owner's
`bottom` store in the single total order S. The owner's `top` load comes after that store,
so it must see `t+1` and fall into the CAS path. On AArch64, a seq_cst store compiles to
`stlr`, the same instruction as a release store. A seq_cst load compiles to `ldar`, while an
acquire load may use the weaker `ldapr` (Apple clang 21 at `-O2` emits `ldapr` for acquire
and `ldar` for seq_cst loads). `ldar` provides exactly the store-to-load ordering the
protocol needs and is only slightly stronger than acquire; no cost figure is claimed here.
On x86 the seq_cst store in `pop` becomes an `xchg`, which is what the fence would have
cost.

`try_steal` returns `kSuccess`, `kEmpty` or `kLostRace`. A lost race means some other
thread took an element (lock-freedom). The pool retries the same victim on `kLostRace`, so
a contended victim is never mistaken for an empty one.

## ThreadPool

### Threading model

The pool has N workers, and each owns a deque, a `Parker` and a random number generator.
A thread-local pointer identifies the calling worker.

* `schedule` from a worker of this pool pushes to that worker's deque (LIFO: the task
  just spawned is the cache-hot one).
* `schedule` from any other thread pushes to a mutex-protected injection queue. An atomic
  count lets workers skip the lock when the queue is empty.
* Worker loop: own deque → steal (random start victim, every other worker tried once) →
  injection queue → up to 32 rounds of `yield` + retry → park.

### Parking without lost wakeups

```
sleeper                                     producer
1. sleepers.fetch_add(1)        seq_cst     a. publish work (bottom store / injected_count++)  seq_cst
2. key = epoch                  acquire     b. if sleepers.load() == 0: done                   seq_cst
3. re-check all queues + wait condition     c. lock; epoch++; pop one idle worker; unlock
4. lock; if epoch != key: abort;            d. unpark it
   push self on idle list; unlock
5. parker.park()
6. lock; leave idle list; unlock;
   if a producer had removed us: search again
```

All of 1, 3, a and b are seq_cst. In the single total order, either the producer's load in
(b) sees the announcement from (1), or the sleeper's re-check in (3) sees the work from (a).
If the producer wakes someone, the epoch bump in (c) happens under the same mutex as check
(4). A sleeper that has not reached (4) yet sees the changed epoch and does not sleep. A
sleeper already past (4) is on the idle list and gets unparked. `Parker` is a flag plus a
condition variable, so an unpark that arrives before `park()` is remembered. When everyone
is busy, `sleepers == 0` and producers never touch the sleep mutex.

Step (6) closes a gap that the first version had. A helping waiter parks with "my event is
set" as its wake condition, and the event's setter unparks it directly. Until the waiter
re-locks the sleep mutex it is still on the idle list. A producer that runs in that window
pops this already-awake worker and spends its single wakeup on it. Without (6) the waiter
saw its event set and left its wait without looking for the producer's task, while the
workers that were really parked stayed asleep with the task queued. If the awake workers
then blocked on that task, the pool deadlocked. So removal from the idle list now carries an
obligation: a worker that wakes and finds that a producer already removed it calls
`find_task` once more and runs what it finds. The regression test
`TaskGroupTest.WakeupSpentOnAwakeWaiterIsNotLost` builds that interleaving with three
workers, ordered only through atomics and `stats().parks`; before the fix it hung.

### Helping waits

`TaskGroup::wait` and `TaskHandle::get` go through `Executor::wait_for(event)`.

* **On a worker of this pool:** the worker loops: run a task from its deque, a victim or
  the injection queue, else spin briefly, else register its own `Parker` in the event (only
  before the first park) and park with "event is set" as the wake condition. Registering
  lazily matters on the hot path: in fork-join the waiter usually pops and runs its own
  child, and an eager registration made that child's completion unpark the waiter's own
  Parker (one self-unpark per join), leaving a stale permit that cut its next sleep short.
  Registration still precedes park()'s final check of the event, so no wakeup is lost. While a worker waits it keeps
  executing other tasks, so depth-first fork-join cannot deadlock, even with a single
  worker. The child it waits for is either in its own deque (it pops and runs the child)
  or running on another worker.
* **On any other thread (main thread, a worker of a different pool):** the thread blocks
  on a stack-allocated `Parker` and does not run tasks.

`CompletionEvent` is a single word: `0` (not set), `1` (set) or a `Parker*` (one waiter).
The waiter registers with a CAS from 0 to its Parker. The setter does
`exchange(1, acq_rel)` and unparks the Parker it got back, if any. Because there is exactly
one RMW on each side, a wakeup cannot be lost. The completer's last access to the event's
memory is the exchange, so the waiter may free the event (a stack `TaskGroup`) as soon as
it sees `1`. The unpark targets the Parker, which lives on the waiting thread or in the pool.
`Parker::unpark` notifies while holding the Parker's mutex, so a stack Parker cannot be
destroyed in the middle of `notify_one`.

A worker's single Parker serves both idle sleep and event waits. A stale unpark (for
example from an event the worker already stopped waiting for) causes one spurious wakeup,
after which the loop re-checks. It is never lost and never wrong.

### TaskGroup counting

`pending` starts at 1 (the owner's token). `spawn` increments it with relaxed ordering:
the increment is sequenced before `schedule`, which publishes the task. Each child
decrements with `acq_rel` after it finishes. `wait()` first checks `pending == 1`
(acquire); if that holds, every child has finished. Otherwise it drops the token, and if
children are still running, the last decrement to 0 sets the event. While the owner holds
the token the count cannot reach 0, so a group that empties and refills during spawning
never sets the event early. After `wait()` the group is re-armed and can be reused. The
first exception (in completion order) is kept and rethrown. The other tasks still run.
The destructor waits and discards an unobserved exception.

### Shutdown (drain)

`~ThreadPool` sets `stopping` and wakes every parked worker. A worker does not exit just
because it finds nothing to do, because a running task may still spawn work or wait for
stolen children. After `stopping`, a worker about to park in its main loop counts itself
in `main_idle`. When all N workers are parked in the main loop at once, the pool is
quiescent:

* no task is running;
* every deque is empty (a worker parks only with an empty deque, and only the owner
  pushes to it);
* the injection queue is empty.

That last worker sets `exiting` and wakes everyone, and the destructor joins the threads.
Guarantee: every task scheduled before the destructor started, and every task those tasks
spawn, runs before the destructor returns. Scheduling from a non-worker thread after
destruction has started, and destroying a pool from one of its own workers, are not
allowed. A `TaskHandle` may outlive its pool: once the task has run, the handle owns the
result.

## Known limitations

* Helping runs arbitrary tasks on the waiting worker's stack, so stack depth can grow
  beyond the recursion depth of the program. The tests run a 300-level chain of nested
  groups on a single worker (under ASan) within the default 512 KiB secondary-thread stack
  on macOS.
* Helping also means the waiting frame cannot resume until the nested task it picked up
  returns. Rule for users: a task must not block on another task's continuation (code that
  runs after some other `wait()`/`get()` returns), and must not hold a lock across
  `wait()`/`get()` on a worker thread. Breaking it can deadlock the pool even with idle
  workers: worker A waits on its group, picks up an unrelated task X inside that wait, and
  X waits for a flag that A only sets after its wait returns. Helping waits deliberately
  still take injected tasks: excluding them would strand external work on a one-worker pool
  whose only worker is waiting on it.
* `CompletionEvent`, and so `TaskHandle::wait()`, supports one waiting thread at a time,
  although `wait()` is `const`. A second concurrent waiter asserts in debug builds.
* Being inside a wait on a pool's handle or group while that pool's destructor runs is
  undefined behaviour (the virtual `wait_for` call can reach a destroyed pool). Waiting
  after the destructor has returned is fine, because every handle is ready by then.
* The injection queue is checked after stealing. A computation that keeps generating
  local work can delay externally submitted tasks until local work runs out.
* Each task costs one heap allocation. A per-worker free list would remove it.

## Task graphs (dependencies)

`include/stealpool/task_graph.hpp` (header-only) lets a caller express a multi-step job as a
DAG: `auto a = g.emplace(f); auto b = g.emplace(h); a.precede(b); g.run(pool).wait();`.
`succeed()` adds edges in the other direction, `emplace(f, g, h)` returns a tuple of handles.
`examples/pipeline_graph.cpp` builds a generate → per-shard sort → per-shard histogram →
merge pipeline once and runs it several times.

* **Built on the executor interface only.** A node is a `detail::Task` subclass, scheduled with
  `Executor::schedule`; the run's completion is a `CompletionEvent` waited for with
  `Executor::wait_for`. `run()` is templated over the pool type (`requires
  std::derived_from<Pool, Executor>`), so the same graph runs on `ThreadPool` and on
  `SharedQueuePool` (both are tested). No hook in the scheduler core was needed.
* **Nodes are their own task objects.** The graph owns the node objects; the node's run
  function does not free it. A run therefore performs no allocation (beyond what the pool's
  queues do), and the same node object is scheduled once per run.
* **Counting.** Each node stores its predecessor count (fixed at build time) and an atomic
  `pending_` reset to it at the start of every run (relaxed: the resets are sequenced before
  the `schedule()` of the roots, and every node runs after one of those calls). A finished node
  does `pending_.fetch_sub(1, acq_rel)` on each successor; the thread that brings a count to zero
  schedules that successor itself, so on `ThreadPool` it lands on that worker's own deque and
  idle workers steal the rest. The release half hands the predecessor's writes to the successor,
  the acquire half on the last decrement gathers all predecessors' writes.
* **Completion.** A graph-wide `remaining_` counter (nodes not yet finished) is decremented
  (acq_rel) by every node as its very last access to the graph, after scheduling its
  successors; the decrement to zero sets the event, exactly like `TaskGroup`'s counter. When the
  waiter sees the event the graph is quiescent, so it can be modified, re-run or destroyed.
* **Exceptions.** The first exception (completion order) is kept; a node that threw, or was
  skipped, sets `skip_` on each successor before decrementing its count, so exactly the nodes
  that transitively depend on a failure are skipped (their callable is never invoked) while
  independent branches still run. Skipped nodes still pass through the scheduler so the counts
  stay exact. `wait()` rethrows; `skipped_in_last_run()` reports how many were skipped.
  `wait()` takes the exception out of the graph *before* it clears `running_`: clearing the
  flag hands the graph to the next `run()`, possibly on another thread, which resets the
  error state. (The opposite order lost or double-freed the exception when two threads took
  turns on one graph; `TaskGraph.NextRunMayStartWhileTheLastWaitRethrows` covers it.)
* **Cycles** are detected with Kahn's algorithm when `run()` sees the graph changed since the
  last check (`GraphCycleError`, nothing runs); `has_cycle()`/`check()` expose the test while
  building and a self-edge is rejected immediately. Duplicate edges are allowed and counted
  consistently on both sides.
* **Re-running and rules.** One run per graph at a time (`run()` on a running graph, or
  `emplace`/`precede`/`clear` during a run, throws `std::logic_error`); any number of runs one
  after another; different graphs may run concurrently on one pool. `RunHandle`'s destructor
  waits (discarding an unobserved exception), so a graph can never be destroyed under its run.
  Nodes may use `TaskGroup`s or run nested graphs on the same pool; waits on a worker help, so
  this works even with one worker (tested). The helping-wait rule (see Known limitations)
  still applies inside nodes: no blocking on another task's continuation, no lock held across
  a wait.
* **Scheduling failures.** Scheduling a ready successor happens inside a `noexcept` task, so
  running out of memory in a pool queue at that point terminates the program. In `run()`
  itself, once the first root is queued the run has started and must settle: if
  `schedule()` throws for a root, that root and the roots after it are completed on the
  calling thread as failed nodes (callables not invoked, dependents skipped, the scheduling
  exception becomes the run's error), and `run()` still returns a handle whose `wait()`
  rethrows. Anything that fails before the first root is queued (cycle check, rebuilding the
  root list) just releases the graph and throws from `run()`.

## SharedQueuePool (benchmark baseline)

`SharedQueuePool` (`include/stealpool/shared_queue_pool.hpp`, `src/shared_queue_pool.cpp`) is
the comparison point for the scaling study in `docs/BENCHMARKS.md`. It implements the same two
`Executor` hooks, so tasks, handles, `TaskGroup`, `parallel_for`, task graphs and the benchmark
workloads run on it unchanged. The only intended difference from `ThreadPool` is the queue:

* **One lock for everything.** A single `std::mutex` guards a `std::deque` of
  `{Task*, pusher index}` entries, the list of parked workers and the shutdown state. Every
  `schedule()` (from a worker or not) and every successful pop takes it. A relaxed `size_` mirror
  lets idle workers poll without the lock, exactly as `ThreadPool` polls its injection queue; it
  is never trusted for a decision to sleep.
* **Order.** A worker takes its own newest queued entry if it has one (found by searching from
  the back; a per-worker count skips the search when it has none), otherwise the oldest entry.
  That is `ThreadPool`'s order with every per-worker deque merged into one: owners pop their
  newest child, thieves take the oldest (largest) task. Helping waits run the task they take on
  top of the waiter's stack, and simpler rules let that nesting run away; each was tried and
  crashed on a recursive `fib(20)` (macOS, 512 KiB worker stacks):
  * oldest always: the waiter's task waits and takes the next oldest, so nesting grows with the
    number of queued tasks (bus error on one worker, Debug);
  * newest always: two waiting workers keep taking each other's newest child (ASan
    stack-overflow on two workers);
  * newest only if this worker pushed it, else oldest: with several workers spawning, the back
    is usually another worker's entry, so waiters took over large old tasks far more often than
    `ThreadPool` steals (ASan stack-overflow).

  With the final rule the fib, sort, tree and spawn-tree tests passed 200 repetitions under
  ASan. `tests/shared_queue_pool_test.cpp` checks the rule itself on one worker (own children
  newest first, external submissions oldest first).
* **Sleeping.** The same idle policy as `ThreadPool` (32 rounds of yield-and-retry, then park).
  A worker re-checks the queue (and, in a helping wait, its event) under the mutex and joins
  the idle list before unlocking; producers push under the same mutex and pop one idle worker
  to unpark. Since both sides hold the lock, no epoch or seq_cst handshake is needed. The
  per-worker `Parker`s replace a shared condition variable: a helping waiter has to wake for
  either new work or its own event, and `CompletionEvent` signals the waiter's `Parker`.
* **Shutdown** drains like `ThreadPool`: after the destructor sets `stopping_`, the worker
  that finds the queue empty and every worker parked in its main loop sets `exiting_` and
  wakes the rest.

## Stress testing

`tests/deque_stress_test.cpp` and `tests/pool_stress_test.cpp` (CTest label `stress`, helpers in
`tests/stress_util.hpp`) are randomized but reproducible. Every random choice derives from one
base seed; each test prints the base seed, every failure carries the derived seed and the
configuration, and every test prints the operation totals it executed.

```sh
ctest --preset tsan -L stress                          # default seed and amount of work
STEALPOOL_STRESS_SEED=0x1234 STEALPOOL_STRESS_SCALE=4 build/tsan/tests/deque_stress_test
```

`STEALPOOL_STRESS_SEED` (decimal or `0x` hex) replaces the base seed; `STEALPOOL_STRESS_SCALE`
multiplies the amount of work (0.1 for a smoke run, 10 for a soak). Thread interleavings are not
reproducible, so a seed reproduces the operation sequence, not the schedule; rerunning a failing
seed several times is the way to reproduce a race.

**Deque level.** Elements are pointers to `Item`s whose plain (non-atomic) `payload` the owner
writes just before the push. Whoever takes an item reads the payload and bumps the item's atomic
`taken` counter. Afterwards every pushed item must have `taken == 1` (no loss, no duplication) and
the right payload; a missing happens-before edge between push and steal is reported by
ThreadSanitizer as a data race on `payload`. Thieves also check that the items they steal from one
deque come in strictly increasing push order (top only moves forward). Scenarios:

* *random program*: the owner runs random phases (push bursts of up to 4096, pop bursts, mixed
  push/pop at a random ratio, drain then push-one/pop-one) against 1, 2, 4 and 7 thieves, three
  seeds each, starting from a 2-slot ring;
* *last-element races*: push 1 to 3 items, pop until empty, repeat, while thieves hammer the same
  slots, so the owner's pop and the thieves' steals keep meeting in the CAS on `top`;
* *forced growth*: the ring is pre-filled past 1000 slots before the thieves start (so growth is
  guaranteed regardless of scheduling), then grows further under concurrent stealing;
* *mesh*: every thread owns a deque and steals from the others when its own runs dry (the
  scheduler's pattern); half of the threads rarely pop, so the others must steal.

**Pool level.** Shapes are pure functions of a seed, so expected results are computed
sequentially and compared with a count and an order-independent hash of the tasks that ran:

* random task trees (fan-out 0..4, depth 9), each node forking through a `TaskGroup`,
  `submit()`+`get()`, `parallel_for` or inline recursion, submitted by 1 or 4 external threads to
  pools of 1..16 workers; one task in 32 is a few thousand times heavier than the rest;
* `TaskGroup`s reused for many rounds, whose tasks spawn into their own group while it is being
  waited on and occasionally throw; owners run concurrently on external threads (blocking waits)
  and as pool tasks (helping waits); every task must run and `wait()` must rethrow one of the
  thrown exceptions exactly in the rounds where something threw;
* `parallel_for` with clustered heavy iterations and nested loops, every index exactly once, for
  several grains;
* external submitters mixing submit/get, detached submits, blocking group waits, `parallel_for`
  from outside, and handles waited on by other pool tasks;
* shutdown racing growing work: detached trees keep submitting from the workers (and running
  nested groups) while the destructor drains; handles kept by the caller are read afterwards;
* two pools where tasks of one block on tasks of the other, which feed work back.

Tests synchronise only through joins, handles and groups; nothing depends on timing.

What the tests deliberately do not do, because the API forbids it: schedule from an external
thread while the destructor runs, or sit inside a wait on a pool's handle or group while that pool
is being destroyed (the waiting call may reach into the pool after it is gone; `thread_pool.hpp`
documents this). Reading a handle after the destructor returned is allowed and tested.
