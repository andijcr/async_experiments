# The loop and timers

`est::loop` is the thing that actually resumes continuations when a promise
is fulfilled or a timer fires. Before it existed (M2), a continuation ran
inline, on whatever call stack called `set_value()`/`set_exception()`. Since
M3, that never happens — `future_state<T>::complete()` and
`set_continuation()` always hand a ready continuation to
`loop.enqueue_ready()` instead, and only `est::loop`'s own drain step ever
actually invokes one. See
[Continuation Node Mechanism](Continuation-Node-Mechanism.md) for the node
side of this; this page covers the loop side.

## Why an explicit `loop&`, not a global singleton

`est::platform::instance()` is a global, swappable pointer — safe, because a
`platform::interface` is stateless policy (a clock read, a `sleep_until`
call, an abort). `est::loop` is not: it owns a ready-queue and a set of
pending timers, real mutable state that a shared global would leak between
any two unrelated call sites (or, worse, between tests). So a loop is an
object a caller constructs explicitly and threads through
`make_promise_future<T>(loop&)` — every `future_state<T>` holds a bare
`loop&`, and every downstream state a `.then()` call creates reuses that
same reference (a chain never crosses loops).

**This makes loop lifetime a real precondition**: a loop must outlive every
`future_state`/`promise`/`future`/`then()`-chain built against it. There's
no way to check a dangling reference at runtime the way `est::check()`
guards other preconditions in this codebase — a caller returning a future
from a function whose loop is a local variable is undefined behavior on the
very next touch of the loop. This is documented explicitly on both
`est::loop`'s and `future_state<T>`'s own doc comments precisely because
it's an easy first-use mistake with no compiler or runtime defense against
it.

## The ready-queue

`loop::enqueue_ready(detail::ready_node&)` pushes onto `ready_`, an
`est::intrusive_list<detail::ready_node>` — the same generic intrusive-list
container `est::mutex` and `est::future_state<T>` each use for their own
queues (see [Architecture](Architecture.md)), templated here on
`detail::ready_node` specifically so `dequeue()` already hands back a
`detail::ready_node*` directly, no cast needed. `run_until_idle()`/`run()`
drain it via `drain_ready()`:

```cpp
void drain_ready() {
  while (auto* node = ready_.dequeue()) {
    run_one(*node);
    if (stop_requested_) {
      return;
    }
  }
}
```

Because the list is re-checked fresh on every iteration, a continuation that
itself completes *another* `future_state` (cascading a further continuation
onto the same ready-queue) gets picked up within the same drain pass — an
arbitrarily deep, purely synchronous chain resolves inside one
`run_until_idle()` call, not one call per link.

`run_one()` does three things around actually invoking the node:

```cpp
void run_one(detail::ready_node& node) {
  const auto guard = destroy_guard(node);          // always destroy, however this exits
  const auto start = platform::instance().now();
  node.run();
  const auto elapsed = platform::instance().now() - start;
  if (elapsed > long_running_threshold) {
    platform::printdbg("est::loop: a continuation took {}ms (> {}ms threshold) to run", ...);
  }
}
```

- **`destroy_guard`** (a `scope_exit` factored into one small helper, shared
  with `fire_ready_timers()`) guarantees the node is deallocated no matter
  how `run()` returns — even though `invoke()` already catches every
  exception a callback could throw internally, so this is a defensive
  guarantee, not something the happy path relies on.
- **Long-running-callback detection**: single-threaded means one slow
  continuation blocks everything else the loop owns — timers, other ready
  work, all of it — with nothing able to preempt it. `run_one()` times every
  invocation against a threshold (currently 50ms, a starting point, not
  tuned against a real workload) and prints a diagnostic via
  `platform::printdbg()` if it's exceeded, so a runaway handler shows up as
  a clear signal instead of "the whole program mysteriously stalled."

## Timers: `schedule_timer()`, and the `future<void>` bridge

`loop` itself knows nothing about futures or promises — it exposes one
primitive, `schedule_timer(detail::timer_node&, deadline)`, and
`est::sleep_for()`/`sleep_until()` (`est:promise`) build the actual
`future<void>`-returning API on top of it (see
[Architecture](Architecture.md#why-loop-doesnt-depend-on-future) for why
this split exists):

```cpp
[[nodiscard]] inline auto sleep_until(loop& loop_ref, loop::clock::time_point deadline)
    -> future<void> {
  auto [prom, fut] = make_promise_future<void>(loop_ref);
  auto fire = [prom = std::move(prom)]() mutable { prom.set_value(); };
  using node_type = detail::concrete_timer_node<decltype(fire)>;
  auto* node = loop_ref.allocator().template new_object<node_type>(std::move(fire));
  loop_ref.schedule_timer(*node, deadline);
  return std::move(fut);
}
```

`schedule_timer()` records the deadline in the M1 `est::timer_queue`
min-heap *and* the node in `loop`'s own `pending_timers_` list, keyed by the
timer queue's own id:

```cpp
void schedule_timer(detail::timer_node& node, clock::time_point deadline) {
  pending_timers_.reserve(pending_timers_.size() + 1);   // see below
  const auto id = timers_.schedule_at(deadline);
  pending_timers_.push_back(pending_entry{.id = id, .node = &node});
}
```

The `reserve()` before `schedule_at()` is a deliberate exception-safety
ordering, not an accident: this method touches *two* separate containers
with no way to roll back a partial update if the second one throws. If
`reserve()` itself throws, `schedule_at()` was never called, so the two
containers stay in sync. Once `reserve()` succeeds, the following
`push_back()` is guaranteed not to reallocate, and `pending_entry` is a
trivial two-member struct that can't itself throw — so there's no window
where `timers_` could know about a deadline that `pending_timers_` doesn't.
(An earlier version of this code didn't have that guarantee, and a
`fire_ready_timers()` `check()` that exists precisely to catch that desync
compiles away entirely under `NDEBUG` — a caught-by-review bug, fixed before
it could turn into release-build undefined behavior.)

`run_impl()`'s main loop sleeps for exactly as long as the *next* deadline,
then fires everything that's ready:

```cpp
void run_impl() {
  ...
  for (;;) {
    drain_ready();
    if (stop_requested_) { return; }
    const auto deadline = timers_.next_deadline();
    if (!deadline) { return; }                        // idle - nothing left to do
    platform::instance().sleep_until(*deadline);
    fire_ready_timers();
  }
}
```

`platform::instance().sleep_until()` is why a test doesn't have to actually
wait real wall-clock time for a timer-driven test to complete: a fake
platform overrides it to advance its own fake clock instantly instead of
blocking (mirroring the same seam `now()`/`assert_failure()` already use —
see `est/tests/loop_tests.cpp`'s `fake_platform`). `fire_ready_timers()`
pops every timer whose deadline has passed, looks it up in `pending_timers_`
to find its node, and calls `fire()` — which for a `sleep_for()`-created
node just does `prom.set_value()`, which in turn triggers `future_state<
void>::complete()`, which enqueues *its* continuations onto the same
ready-queue `drain_ready()` will pick up on the loop's next iteration.

## `run()` vs. `run_until_idle()`, and `stop()`

Both currently do exactly the same thing — drain ready work, sleep until the
next deadline, repeat, until idle (no ready work and no pending timers) or
`stop()` is called. The difference the design anticipates (`run()` blocking
indefinitely, kept alive by a live I/O reactor with more wakeup sources than
timers) only becomes real once I/O support lands — explicitly out of scope
for this milestone. Until then nothing could ever wake a fully idle loop
back up anyway (no I/O, single-threaded), so returning is the only sane
behavior for either name. This is documented as a stated fact in the code
rather than left implicit.

`stop()` sets a flag checked after every continuation and every ready-queue
drain pass — it's how a caller (or, eventually, M4's coroutine machinery)
asks the loop to return early, before it would otherwise go idle on its own.

Reentering `run()`/`run_until_idle()` on the *same* loop while already
inside one — e.g. a continuation that calls `stop()` and then calls
`run_until_idle()` again before returning — is a checked precondition
violation (`check(!running_, ...)`), not silently tolerated: without the
check, the nested call's own "reset `stop_requested_` to false on entry"
step would wipe out the outer, still-in-progress `stop()` request out from
under it. No coroutine machinery exists yet to make nested pumping a real,
supported use case, so this is the same debug-checked-precondition stance
`est::check()` takes everywhere else in this codebase, not new reentrant-
`stop()` bookkeeping built for a use case nothing needs today.
