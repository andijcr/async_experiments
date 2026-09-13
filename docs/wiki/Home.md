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
- **[Coroutines](Coroutines.md)** — why `est::future<T>` itself is the
  coroutine return type (no separate `task<T>`), the `promise_type`/
  `operator co_await()` machinery behind it, why a resume node must be
  separately heap-allocated rather than embedded in the coroutine frame
  it resumes, and how `est::mutex::lock()` is awaitable on the same
  pieces.
- **[Global Lookup Codegen](Global-Lookup-Codegen.md)** — what
  `est::current_loop()`/`.allocator()` actually compile to under LTO:
  real disassembly, instruction counts, and pointer-chase counts, plus
  why whole-program devirtualization doesn't currently help.

## The five-second architecture summary

```
est::platform   — a monotonic clock + "what happens when a check fails" seam,
                   swappable per backend (hosted Linux today; bare metal later)
est::shared_ptr — a single-allocation, non-atomic reference-counted pointer
est::counting_event —
                   an intrusive waiter list + count; wait() is awaitable,
                   set() hands units directly to queued waiters; binary_event/
                   one_shot_event layer stricter constraints on top without
                   re-implementing any of it
est::mutex      — a binary_event<automatic> in disguise; lock() returns an
                   awaitable RAII future<lock_guard>, guarding a critical
                   section across a coroutine suspension
est::timer_queue — a min-heap of deadlines, driven by est::platform's clock
est::loop       — owns a ready-queue and the timer_queue; the only thing that
                   ever actually invokes a continuation or fires a timer
est::future<T>/
est::promise<T> — a thin, shared_ptr-backed producer/consumer pair; .then()
                   chains defer through est::loop instead of running inline;
                   future<T> is also a coroutine's return type (no task<T>)
```

Every future/promise pair is built via `est::make_promise_future<T>()` —
there is no way to pass it (or `est::mutex`, `est::counting_event<Mode>`,
`sleep_for()`/`sleep_until()`/`yield_execution()`, or a coroutine's own
`promise_type`) an explicit `loop&` at all; every one of them resolves
`est::current_loop()`/`est::current_allocator()`
([Loop and Timers](Loop-And-Timers.md)) fresh, at the point of use,
instead - a pair of `thread_local` reads, one per core (or, hosted, per
thread), not a runtime-swappable global. A loop only becomes "current" by
an explicit `est::make_current_loop(loop)` call (a free function, not a
method on `est::loop` itself - deliberately kept out of `loop.cppm`
entirely), scoped to that loop's own lifetime; there is no fallback for a
caller that never registers one - `current_loop()`/`current_allocator()`
fail their precondition instead. See
[Loop and Timers](Loop-And-Timers.md) for the correctness hazard an
earlier, non-`thread_local` version of this design carried (and the fix
for its sharpest form) and
[Global Lookup Codegen](Global-Lookup-Codegen.md) for what each version
actually costs, measured.

`import est;` doesn't install a `platform::interface` on its own, either —
in fact it doesn't even know a concrete backend exists. `estext` is a
second, wholly separate module holding `hosted_stdcpp`, the one that does
([Architecture](Architecture.md) has the full story on why it's not part
of `est` at all); a program's own `main()` does `import estext;`,
constructs one, and `platform::override_instance()`s it
(`examples/hello_world/main.cpp`, `examples/sleep_sort/main.cpp`), same as
`est/tests/`'s own test binary does once, in `est/tests/test_main.cpp`.

## Where to look in the source

| Concept | File |
|---|---|
| Platform seam (`platform::interface`, `instance()`/`override_instance()`, `printdbg`, `get_random_seed()`) | `est/src/platform/platform.cppm` |
| `hosted_stdcpp` (the one concrete `platform::interface` - a separate module, `estext`, not part of `est`) | `estext/src/hosted_stdcpp.cppm` |
| `est::check()` | `est/src/check.cppm` |
| `est::jitter` | `est/src/util/jitter.cppm` |
| `est::shared_ptr<T>`, `est::ref_counted`, `detail::shared_ptr_common<Derived, Pointer, T>`, `detail::shared_ptr_control_block<T>` | `est/src/util/shared_ptr.cppm` |
| `est::intrusive_list_node`, `est::intrusive_list<T>` | `est/src/util/intrusive_list.cppm` |
| `est::counting_event<Mode>`, `est::binary_event<Mode>`, `est::one_shot_event<Mode>`, `EventResetMode` | `est/src/sync/event.cppm` |
| `est::external_event<T>` | `est/src/sync/external_event.cppm` |
| `est::mutex`, `mutex::lock_guard` (built on `est::binary_event<EventResetMode::automatic>`) | `est/src/sync/mutex.cppm` |
| `est::timer_queue<Allocator>` | `est/src/timer.cppm` |
| `est::loop`, `ready_node`, `timer_node`, `detail::abandoned_exception` | `est/src/loop.cppm` |
| `est::current_loop()`, `est::current_allocator()`, `est::make_current_loop()` | `est/src/util/current_loop.cppm` |
| `est::schedule_periodic()`, `est::periodic_timer_handle`, `detail::periodic_timer_node<Fn>` | `est/src/timer_periodic.cppm` |
| `est::future_state<T>`, `est::future<T>` (incl. `clone()`), `continuation_node<T>`, `future<T>::promise_type`, coroutine awaiters | `est/src/future.cppm` |
| `est::promise<T>`, `make_promise_future()`, `make_ready_future()`, `sleep_for()`/`sleep_until()`, `detail::promise_resume_node<T>` | `est/src/promise.cppm` |
| `est::when_all()` (fixed-arity and `std::span` overloads), `detail::when_all_state` | `est/src/when_all.cppm` |
| `est::when_any()` (fixed-arity and `std::span` overloads) | `est/src/when_any.cppm` |
| `est::when_any_succeeds()` (fixed-arity and `std::span` overloads), `detail::when_any_succeeds_state` | `est/src/when_any_succeeds.cppm` |
