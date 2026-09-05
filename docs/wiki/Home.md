# est — a single-threaded async framework for C++23

`est` is an educational, single-threaded async framework built from scratch on
C++23 modules: no threads, no atomics, no locks that actually protect
anything yet — just the building blocks (a monotonic clock seam, a run loop,
a future/promise pair, a reference-counted pointer) assembled the way a real
framework would be, with every design decision made and documented as it was
needed rather than copied from an existing library.

This wiki is a reader's guide to *how the code works* — the mechanisms, the
allocation behavior, the ownership model. For the *history* of how it got
this way (every milestone, every reviewed design decision, every bug found
and fixed, with dates and reasoning) see [`docs/PLAN.md`](../PLAN.md) in the
repository root — it is the project's full decision log and is kept
scrupulously up to date. This wiki summarizes and cross-references it rather
than duplicating it.

## Pages

- **[Architecture](Architecture.md)** — the module layout, the dependency
  graph between partitions, and the design philosophy (single-threaded,
  allocator-first, virtual-dispatch platform seam) that shapes everything
  else.
- **[Continuation Node Mechanism](Continuation-Node-Mechanism.md)** — how
  `then()` actually works under the hood: the `ready_node` /
  `continuation_node<T>` / `concrete_continuation<Fn, U>` type hierarchy,
  the wrapped-vs-unwrapped calling conventions, and how monadic flattening
  is just an ordinary `then()` call in disguise.
- **[Allocation Patterns](Allocation-Patterns.md)** — exactly how many heap
  allocations a promise/future pair, a `.then()` call, a timer, and a
  flattened chain cost, with a worked example tracing every allocation and
  deallocation through a multi-stage chain.
- **[The Loop and Timers](Loop-And-Timers.md)** — how `est::loop` actually
  schedules and runs continuations, how `sleep_for()`/`sleep_until()` bridge
  timers into futures, and the long-running-callback / reentrancy guards.

## The five-second architecture summary

```
est::platform   — a monotonic clock + "what happens when a check fails" seam,
                   swappable per backend (hosted Linux today; bare metal later)
est::shared_ptr — a single-allocation, non-atomic reference-counted pointer
est::mutex      — an intrusive waiter list + lock word (bookkeeping only —
                   nothing is concurrent yet)
est::timer_queue — a min-heap of deadlines, driven by est::platform's clock
est::loop       — owns a ready-queue and the timer_queue; the only thing that
                   ever actually invokes a continuation or fires a timer
est::future<T>/
est::promise<T> — a thin, shared_ptr-backed producer/consumer pair; .then()
                   chains defer through est::loop instead of running inline
```

Every future/promise pair is built via `est::make_promise_future<T>(loop&)` —
an explicit `est::loop&` is threaded through everything (not a global
singleton), so a caller owns exactly when and where continuations actually
run.

## Where to look in the source

| Concept | File |
|---|---|
| Platform seam (clock, `sleep_until`, `assert_failure`, `printdbg`) | `est/src/platform/platform.cppm` |
| `est::check()` | `est/src/check.cppm` |
| `est::shared_ptr<T>`, `enable_shared_from_this<T>` | `est/src/util/shared_ptr.cppm` |
| `est::intrusive_list_node`, `est::intrusive_list<T>` | `est/src/util/intrusive_list.cppm` |
| `est::mutex`, `mutex_waiter` | `est/src/sync/mutex.cppm` |
| `est::timer_queue<Allocator>` | `est/src/timer.cppm` |
| `est::loop`, `ready_node`, `timer_node` | `est/src/loop.cppm` |
| `est::future_state<T>`, `est::future<T>`, `continuation_node<T>` | `est/src/future.cppm` |
| `est::promise<T>`, `make_promise_future()`, `sleep_for()`/`sleep_until()` | `est/src/promise.cppm` |
