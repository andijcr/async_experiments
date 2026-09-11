# The loop and timers

`est::loop` is the thing that actually resumes continuations when a promise
is fulfilled or a timer fires. A continuation never runs inline, on whatever
call stack called `set_value()`/`set_exception()` — `future_state<T>::complete()`
and `set_continuation()` always hand a ready continuation to
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

## `est::current_loop()`: opt-in, not automatic, and not `thread_local`

A caller can still avoid threading a `loop&` through by hand, without
reopening the global-singleton problem the section above rules out -
`est::make_current_loop(loop&)` registers that loop as the one
`make_promise_future()`, `sleep_for()`/`sleep_until()`/`yield_execution()`,
and a loop-less coroutine's own `promise_type` (all consuming
`est::current_loop()`) fall back to, until the returned guard is
destroyed:

```cpp
// est:util.current_loop - free functions, not methods on est::loop itself
[[nodiscard]] auto make_current_loop(loop& loop_ref) noexcept {
  check(!detail::loop_is_current, "est::make_current_loop(): another loop is already current...");
  detail::loop_is_current = true;
  platform::instance().set_current_loop_context(&loop_ref);
  return scope_exit([]() noexcept {
    detail::loop_is_current = false;
    platform::instance().set_current_loop_context(nullptr);
  });
}

[[nodiscard]] auto current_loop() -> loop& {
  auto* const context = platform::instance().get_current_loop_context();
  check(context != nullptr, "est::current_loop(): no loop is current ...");
  return *context;
}
```

A few design choices worth understanding, each driven by a real
constraint:

- **Not `thread_local`.** This codebase already has a plain,
  non-`thread_local` mechanism for "which one is current" -
  `platform`'s own `current_instance`/`instance()` - and reusing that
  shape for the current loop costs nothing extra. `thread_local` is
  never otherwise needed here, and the bare-metal stretch goal
  (freestanding, no OS) is exactly the kind of target where
  `thread_local` may have no well-defined support at all.
- **The slot lives behind `platform::interface`'s virtual
  `get_current_loop_context()`/`set_current_loop_context()`, not as a
  free-standing global next to `current_instance`.** `platform::interface`
  is a pure interface - every method pure virtual, no data members of
  its own - so "hold a pointer and hand it back" is implemented by
  whichever concrete backend is installed (`estext::hosted_stdcpp`
  today), the same as `reset_loop_stall_detection()`/
  `detect_loop_stall()` (the long-running-callback detector). Every test
  fake in `est/tests/` deriving from `interface` provides matching
  overrides - most are no-ops, but `loop_tests.cpp`'s
  `fake_platform`/`jumping_platform` need working ones, since `est::loop`
  is genuinely constructed under both.
- **Registration is an explicit, RAII-scoped opt-in
  (`make_current_loop()`), not automatic constructor/destructor
  registration.** Creating a loop and deciding it should be "the"
  current one are two separate concerns - most loops in this codebase's
  own tests are never meant to be "the" current loop at all, so forcing
  every one of them through the same nesting-checked slot would be the
  wrong default. `make_current_loop()` is modeled directly on
  `platform::override_instance()`'s own shape. The nesting precondition
  is tracked by a dedicated `detail::loop_is_current` flag in
  `:util.current_loop`, separately from `get_current_loop_context()`'s
  own null/non-null answer - see the next point for why that distinction
  matters.
- **A caller that never wants to think about `est::loop` at all still
  gets a genuinely working one.** Rather than `current_loop()` simply
  failing its precondition until something calls `make_current_loop()`,
  `hosted_stdcpp`'s own `get_current_loop_context()` falls back to a loop
  of its own, lazily constructed on first use, whenever nothing has been
  explicitly registered - driven the same way any other loop is
  (`est::current_loop().run_until_idle();`), without ever writing
  `est::loop loop;` by hand. This is exactly why `hosted_stdcpp` needs to
  name `est::loop` and so can't be one of `est`'s own partitions - see
  [Architecture](Architecture.md)'s `estext` section for the full module
  story, and this file's own top comment for why `:platform` itself
  can't.
- **`import est;` never installs a backend as a side effect.** That
  decision belongs to a program's own entry point, not the library.
  `examples/hello_world/main.cpp` and `examples/sleep_sort/main.cpp`
  each construct a `hosted_stdcpp` and `override_instance()` it at the
  top of their own `main()`; `est/tests/`'s own Catch2 binary does the
  same once, in a small custom `main()` (`est/tests/test_main.cpp`,
  linked against `Catch2::Catch2` rather than `Catch2::Catch2WithMain`)
  instead of every individual test file.
- **The registration mechanism lives in its own partition,
  `est/src/util/current_loop.cppm` (`:util.current_loop`), as free
  functions `est::make_current_loop(loop&)`/`est::current_loop()` - not
  methods on `est::loop` itself.** A primitive ready-queue-and-timers
  type and an opt-in convenience for not threading a `loop&` by hand are
  two unrelated concerns; keeping them apart means `loop.cppm` carries
  no trace of this mechanism. The partition sits above both `:loop`
  (needs the type) and `:platform` (needs `instance()`) in the
  dependency DAG, same shape as `estext` one level up - ordinary, since
  a partition of `est` itself is free to import `:loop` directly (only
  `:platform` itself, and anything that must stay *below* `:loop`,
  can't). `detail::loop_is_current` (the nesting flag) lives in this
  same file, non-exported.

The actual storage is a genuinely typed `est::loop*`, not an opaque
`void*` - `:platform` names the type via an exported forward declaration
without needing to complete it; only the concrete backend that
implements `get_current_loop_context()` needs the complete type, to
actually construct one. No cast needed on either side. The only call
sites that ever write into this slot are `make_current_loop()`'s own
guard and, for `estext::hosted_stdcpp` specifically, its own
lazily-constructed fallback loop.

A single slot with a checked precondition against nesting, not a
push/pop stack like `platform::override_instance()`'s: two loops both
current at once (one calling `make_current_loop()` while another's guard
is still alive) is treated as a programming error (an `est::check()`
failure - see `est/tests/check_tests.cpp`'s own doc comment on why that
failure path isn't unit-tested here), not "the inner one temporarily
shadows the outer."

Every free function that takes an explicit `loop&` has a
matching overload that pulls `est::current_loop()` instead -
`make_promise_future<T>()`, `sleep_for()`/`sleep_until()`,
`yield_execution()` (all `est:promise`) - and `est::mutex`/
`est::counting_event<Mode>` each have a matching no-argument constructor.
A coroutine returning `est::future<T>`
can drop the `est::loop&` parameter the same way - see
[Coroutines](Coroutines.md)'s own calling-convention section for how
`promise_type` resolves that without ambiguity against the original,
loop-taking convention.

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
  platform::instance().reset_loop_stall_detection();
  node.run();
  platform::instance().detect_loop_stall(long_running_threshold);
}
```

- **`destroy_guard`** (a `scope_exit` factored into one small helper, shared
  with `fire_ready_timers()`) guarantees the node is deallocated no matter
  how `run()` returns — even though `invoke()` already catches every
  exception a callback could throw internally, so this is a defensive
  guarantee, not something the happy path relies on.
- **Long-running-callback detection**: single-threaded means one slow
  continuation blocks everything else the loop owns — timers, other ready
  work, all of it — with nothing able to preempt it. Measuring and
  reporting a stall lives on `platform::interface` -
  `reset_loop_stall_detection()`/`detect_loop_stall(threshold)` — for the
  same reason `now()`/`sleep_until()` are platform hooks rather than
  `est::loop` calling `std::chrono`/`std::this_thread` directly: "how do
  we know a callback ran long" is a policy a backend should get to answer
  for itself. `est::loop` only calls the two bracketing hooks and owns the
  threshold value (currently 50ms, a starting point, not tuned against a
  real workload); both are pure virtual on `platform::interface` itself
  (see the `current_loop()` section above for why `interface` carries no
  default bodies or state of its own at all), so each concrete backend
  answers them itself. `hosted_stdcpp`'s own override just records
  `now()` on reset and compares against it on detect, printing via
  `platform::printdbg()` if exceeded. A future backend could implement
  both to run a watchdog on a background thread instead, catching (and
  reporting) a stall in parallel while the callback is still running,
  rather than only finding out once it returns — without `run_one()`
  itself changing at all.

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
  auto* node = loop_ref.allocator().template new_object<detail::sleep_resume_node>(std::move(prom));
  loop_ref.schedule_timer(*node, deadline);
  return std::move(fut);
}
```

`sleep_resume_node` holds the `promise<void>` directly rather than
wrapping a generic closure - needed so `destroy(allocator, ran)` can
complete the promise with an exception when `ran` is false (the timer
never fired before the loop was destroyed). A generic, type-erased
callback would give `destroy()` no way to know it's holding a promise at
all, and a silent drop instead would have exactly the
stranded-coroutine-frame hazard described in
[Coroutines](Coroutines.md)'s `lock_resume_node`/`acquire_resume_node`
section.

`yield_execution(loop&)` gives `loop_ref` the chance to run
whatever else is already ready before the calling coroutine resumes. It
has no real deadline to track, so it's a direct
`detail::yield_resume_node` handed straight to `loop_ref.enqueue_ready()`
- a plain `ready_node` holding a `promise<void>`, no timer machinery at
all:

```cpp
[[nodiscard]] inline auto yield_execution(loop& loop_ref) -> future<void> {
  auto [prom, fut] = make_promise_future<void>(loop_ref);
  auto* node = loop_ref.allocator().template new_object<detail::yield_resume_node>(std::move(prom));
  loop_ref.enqueue_ready(*node);
  return std::move(fut);
}
```

FIFO is what makes this safe: `enqueue_ready()` appends at the tail, so
everything already queued when `yield_execution()` is called runs first
(see [Architecture](Architecture.md) and `intrusive_list<T>`'s own doc
comment for why the list is FIFO). `yield_resume_node::destroy()` completes its
promise with an exception on abandonment rather than silently dropping
it, for the same reason `mutex::lock_resume_node`'s own doc comment
gives (`docs/wiki/Coroutines.md`) - a coroutine suspended via `co_await
yield_execution(loop);` holds the only other reference to its
`future_state<void>`, so silently dropping the promise would strand that
coroutine's frame forever if the loop is destroyed first.

`schedule_timer()` records the deadline in `est::timer_queue`'s own
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
`fire_ready_timers()`'s own `check()` exists to catch that desync if it
ever happened, but compiles away entirely under `NDEBUG` — the ordering
above is what makes the desync unreachable in the first place, not just
debug-detected.

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
timers) only becomes real once I/O support exists. Until then nothing
could ever wake a fully idle loop back up anyway (no I/O,
single-threaded), so returning is the only sane behavior for either
name. This is documented as a stated fact in the code rather than left
implicit.

`stop()` sets a flag checked after every continuation and every ready-queue
drain pass — it's how a caller asks the loop to return early, before it
would otherwise go idle on its own.

Reentering `run()`/`run_until_idle()` on the *same* loop while already
inside one — e.g. a continuation that calls `stop()` and then calls
`run_until_idle()` again before returning — is a checked precondition
violation (`check(!running_, ...)`), not silently tolerated: without the
check, the nested call's own "reset `stop_requested_` to false on entry"
step would wipe out the outer, still-in-progress `stop()` request out from
under it. Nested pumping isn't a supported use case, so this is the same
debug-checked-precondition stance `est::check()` takes everywhere else
in this codebase, not new reentrant-`stop()` bookkeeping built for a use
case nothing needs today.
