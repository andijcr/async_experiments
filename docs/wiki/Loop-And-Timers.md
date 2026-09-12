# The loop and timers

`est::loop` is the thing that actually resumes continuations when a promise
is fulfilled or a timer fires. A continuation never runs inline, on whatever
call stack called `set_value()`/`set_exception()` — `future_state<T>::complete()`
and `set_continuation()` always hand a ready continuation to
`loop.enqueue_ready()` instead, and only `est::loop`'s own drain step ever
actually invokes one. See
[Continuation Node Mechanism](Continuation-Node-Mechanism.md) for the node
side of this; this page covers the loop side.

## Why `est::current_loop()`, not a global singleton

`est::platform::instance()` is a global, swappable pointer — safe, because a
`platform::interface` is stateless policy (a clock read, a `sleep_until`
call, an abort). `est::loop` is not: it owns a ready-queue and a set of
pending timers, real mutable state that a shared global would leak between
any two unrelated call sites (or, worse, between tests). So a loop is an
object a caller constructs explicitly.

There is, deliberately, no way to hand a loop to `make_promise_future()`,
`sleep_for()`/`sleep_until()`/`yield_execution()`, `est::mutex`,
`est::counting_event<Mode>`, or a coroutine's own `promise_type` directly —
every one of them resolves `est::current_loop()` fresh, at the point of
use, instead of taking or caching a `loop&` of their own (see the next
section for the mechanism, and each type's own doc comment for the
cross-loop hazard this carries). This is itself the result of an
experiment (branch `current-loop-only-experiment`) that started from an
earlier design where every one of those also accepted an explicit `loop&`
and cached it — see [Global Lookup Codegen](Global-Lookup-Codegen.md) for
the codegen side of that comparison, and the "A structural hazard:
destroying a loop with pending work" section below for a genuine
correctness regression the change introduced and the fix that closed it.

**This makes loop lifetime a real precondition, now transitively through
`current_loop()` rather than through a cached member**: a loop must outlive
everything that ever resolved it via `current_loop()` — every
`future_state`/`promise`/`future`/`then()`-chain, every `est::mutex`,
every `est::counting_event<Mode>`. There's no way to check a dangling
reference at runtime the way `est::check()` guards other preconditions in
this codebase.

## `est::current_loop()`/`current_allocator()`: `thread_local`, opt-in, no fallback

`est::make_current_loop(loop&)` registers a loop as the one
`make_promise_future()`, `sleep_for()`/`sleep_until()`/`yield_execution()`,
`est::mutex`, `est::counting_event<Mode>`, and a coroutine's own
`promise_type` (all consuming `est::current_loop()`/`current_allocator()`)
resolve, until the returned guard is destroyed:

```cpp
// est:util.current_loop - free functions, not methods on est::loop itself
namespace est::detail {
struct execution_context {
  loop* loop_ptr = nullptr;
  std::pmr::memory_resource* resource_ptr = nullptr;
};
inline thread_local execution_context tls_context;
} // namespace est::detail

[[nodiscard]] auto make_current_loop(loop& loop_ref) noexcept {
  check(detail::tls_context.loop_ptr == nullptr, "est::make_current_loop(): another loop is already current...");
  detail::tls_context = {.loop_ptr = &loop_ref, .resource_ptr = loop_ref.allocator().resource()};
  return scope_exit([&loop_ref]() noexcept {
    loop_ref.drain_pending(); // see the hazard section below for why
    detail::tls_context = {};
  });
}

[[nodiscard]] auto current_loop() -> loop& {
  check(detail::tls_context.loop_ptr != nullptr, "est::current_loop(): no loop is current ...");
  return *detail::tls_context.loop_ptr;
}

[[nodiscard]] auto current_allocator() -> std::pmr::polymorphic_allocator<std::byte> {
  check(detail::tls_context.resource_ptr != nullptr, "est::current_allocator(): no loop is current ...");
  return {detail::tls_context.resource_ptr};
}
```

This replaced an earlier version of the mechanism that routed through
`platform::interface`'s own virtual `get_current_loop_context()`/
`set_current_loop_context()` methods (removed) and fell back to a
lazily-constructed default loop when nothing was explicitly registered
(also removed) - see
[Global Lookup Codegen](Global-Lookup-Codegen.md#the-threadlocal-migration)
for why, and the measured cost difference. A few design choices worth
understanding in the current version, each driven by a real constraint:

- **`thread_local`, not a process-global.** This codebase's ultimate
  target is a shared-memory, no-MMU multicore machine with one loop per
  core - a plain global would need every core to agree on (and
  synchronize writes to) one slot, even though each core only ever
  registers and reads its own loop. `thread_local` gives each core (or,
  hosted, each thread) an independent slot for the price of one
  relative-addressed load, no synchronization needed -
  `est::platform::detail::current_instance` (`:platform`) now uses the
  same storage strategy, for the same reason.
- **Two separate fields (`loop_ptr`, `resource_ptr`), not just the loop
  pointer alone.** `current_allocator()` reads `resource_ptr` directly
  rather than going through `current_loop().allocator()`, which would
  need a further memory read through the loop pointer to reach the same
  value - most internal callers (`future_state<T>`'s own `allocator()`,
  a coroutine frame's `operator new`/`operator delete`,
  `make_promise_future()`) only ever need the allocator, not the loop
  itself, so this is the more common path, not a rarely-used shortcut.
- **No fallback loop.** An earlier version of `hosted_stdcpp` lazily
  constructed a default loop and handed it out whenever nothing had been
  explicitly registered, so a caller that never wanted to think about
  `est::loop` at all still got a genuinely working one. Removed along
  with the virtual `get_current_loop_context()` it depended on: on the
  target this mechanism now designs for, there is no "hosted" convenience
  backend and no notion of a made-up default loop for a core that forgot
  to register its own - every core (and every hosted test/example) must
  register one explicitly, and `current_loop()`/`current_allocator()`
  fail their precondition immediately (aborting, in a checked build)
  rather than silently substituting a different loop.
- **Registration is an explicit, RAII-scoped opt-in
  (`make_current_loop()`), not automatic constructor/destructor
  registration.** Creating a loop and deciding it should be "the"
  current one are two separate concerns - most loops in this codebase's
  own tests are never meant to be "the" current loop at all, so forcing
  every one of them through the same nesting-checked slot would be the
  wrong default. `make_current_loop()` is modeled directly on
  `platform::override_instance()`'s own shape, and deliberately kept as
  a *separate* call from it (not unified into one combined "install
  everything" registration) - see this file's own top comment on
  `:util.current_loop` for why: some callers (most of
  `est/tests/platform_tests.cpp`/`timer_tests.cpp`) want to override the
  platform with no loop involved at all, and some callers install the
  platform once for an entire program while registering a different loop
  per operation (`est/tests/loop_tests.cpp`) - two independent,
  composable registrations fit both shapes; one combined one fits
  neither as well.
- **The registration mechanism lives in its own partition,
  `est/src/util/current_loop.cppm` (`:util.current_loop`), as free
  functions - not methods on `est::loop` itself.** A primitive
  ready-queue-and-timers type and an opt-in convenience for not threading
  a `loop&` by hand are two unrelated concerns; keeping them apart means
  `loop.cppm` carries no trace of this mechanism. The partition only
  needs `:check`, `:loop`, and `:util.scope_exit` now - it no longer
  needs `:platform` at all, since the loop's own registration no longer
  routes through it.

A single slot with a checked precondition against nesting, not a
push/pop stack like `platform::override_instance()`'s: two loops both
current at once (one calling `make_current_loop()` while another's guard
is still alive, on the same thread/core) is treated as a programming
error (an `est::check()` failure - see `est/tests/check_tests.cpp`'s own
doc comment on why that failure path isn't unit-tested here), not "the
inner one temporarily shadows the outer."

Every one of `make_promise_future<T>()`, `sleep_for()`/`sleep_until()`,
`yield_execution()` (all `est:promise`), `est::mutex`'s and
`est::counting_event<Mode>`'s constructors, and a coroutine's own
`promise_type` (`est:future`) resolves `current_loop()` this way - there is
no explicit-`loop&`-taking alternative left for any of them (see
[Coroutines](Coroutines.md)'s own calling-convention section for the one
wrinkle this leaves behind: a coroutine can still take an `est::loop&`
*parameter*, but nothing reads it any more). See
[Global Lookup Codegen](Global-Lookup-Codegen.md) for what going through
`current_loop()` actually costs, in real, disassembled instructions,
against the cached-`loop&` design this one replaced.

## A structural hazard: destroying a loop with pending work

Resolving `current_loop()` fresh at every point of use instead of caching
a `loop&` member (`future_state<T>`, `est::mutex`, `est::counting_event`)
or a `memory_resource*` (a coroutine frame's own `operator delete`, see
[Continuation Node Mechanism](Continuation-Node-Mechanism.md)) opens a real
gap: any of those lookups can resolve to a *different* loop than the one
that originally created the thing being destroyed, if the current-loop
registration changed in between. In the ordinary case that's just a
documented precondition, not something that bites in practice - every
existing caller keeps a single loop current for the lifetime of everything
it creates.

`est::loop` itself broke that assumption, and did so in code this
codebase's own test suite already exercised: the "leaks nothing" family of
tests (`est/tests/{mutex,event,loop}_tests.cpp`) deliberately destroys a
loop while a coroutine is still suspended on it, to prove nothing leaks.
The idiom every one of those tests (and every example) uses is

```cpp
est::loop loop;
const auto loop_guard = est::make_current_loop(loop);
// ... schedule work, possibly abandon some of it ...
```

C++ destroys locals in reverse declaration order, so `loop_guard` -
declared *after* `loop`, because it needs `loop` to already exist - always
runs its destructor *before* `loop`'s. If a still-suspended coroutine's
frame is torn down from inside `~loop()` (via a queued resume node's
`destroy()`, which calls the coroutine handle's own `destroy()`, which
calls the frame's `operator delete`, which resolves `current_allocator()`
fresh), that lookup runs *after* `loop_guard` has already cleared the
current-loop slot. The result isn't a documented hazard someone forgot to
avoid - it's a real, reproducible one. At the time this was found, an
earlier version of `current_loop()` still had a fallback loop
(hosted_stdcpp's own, since removed - see the previous section): the
stranded lookup silently resolved to that *different* `est::loop` object,
with its own allocator, and the frame got deallocated through the wrong
`std::pmr::memory_resource`. Observed directly, on this branch, before the
fix below: allocation/deallocation count mismatches in some tests, and
outright heap corruption (`SEGFAULT`/`Subprocess aborted` under plain
`ctest`) in others. With the fallback now gone, the same stranded lookup
would instead fail its precondition immediately and abort - a real
improvement (fail fast instead of silent corruption) but not a fix on its
own: the drain below is still what keeps this from happening at all in
the first place.

**The fix**: `est::loop::drain_pending()` is a new public method - the
same "destroy everything still queued, without running it" logic
`~loop()` already had, factored out and made idempotent - and
`make_current_loop()`'s returned guard now calls `loop_ref.drain_pending()`
as the *first* thing its destructor does, before clearing the current-loop
slot. That drains `loop_ref` (destroying any still-suspended coroutine
frames, among everything else) while it is still the registered current
loop, so every `current_loop()` lookup that cascades out of that drain
resolves correctly. `~loop()` still calls `drain_pending()` itself
unconditionally, so a loop never registered via `make_current_loop()` at
all is unaffected - the method is a no-op the second time (or the only
time) it runs, since by then both containers it drains are already empty.

One real behavioral consequence worth calling out: work scheduled while a
loop was current but never actually run is now destroyed the moment that
loop stops being current, not just whenever the loop itself is eventually
destroyed. Every caller in this codebase already runs a loop to idle
before its guard goes out of scope, so this doesn't change any existing
example's or test's behavior - but it is a real narrowing of what used to
be possible (deferring a loop's remaining work to a later, separate
`make_current_loop()` scope no longer works). See
[Global Lookup Codegen](Global-Lookup-Codegen.md) for the rest of this
experiment's findings.

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
[[nodiscard]] inline auto sleep_until(loop::clock::time_point deadline) -> future<void> {
  auto& loop_ref = current_loop();
  auto allocator = current_allocator();
  auto [prom, fut] = detail::make_promise_future_impl<void>(allocator);
  auto* node = allocator.template new_object<detail::sleep_resume_node>(std::move(prom));
  loop_ref.schedule_timer(*node, deadline);
  return std::move(fut);
}
```

`current_loop()` is still resolved here (`schedule_timer()` needs the
loop itself), but the allocations go through `current_allocator()`
directly rather than `loop_ref.allocator()` - the same "reach the
allocator without a detour through the loop pointer" pattern used
everywhere current_loop() isn't independently needed.

`sleep_resume_node` holds the `promise<void>` directly rather than
wrapping a generic closure - needed so `destroy(allocator, ran)` can
complete the promise with an exception when `ran` is false (the timer
never fired before the loop was destroyed). A generic, type-erased
callback would give `destroy()` no way to know it's holding a promise at
all, and a silent drop instead would have exactly the
stranded-coroutine-frame hazard described in
[Coroutines](Coroutines.md)'s `lock_resume_node`/`acquire_resume_node`
section.

`yield_execution()` gives `current_loop()` the chance to run
whatever else is already ready before the calling coroutine resumes. It
has no real deadline to track, so it's a direct
`detail::yield_resume_node` handed straight to `loop_ref.enqueue_ready()`
- a plain `ready_node` holding a `promise<void>`, no timer machinery at
all:

```cpp
[[nodiscard]] inline auto yield_execution() -> future<void> {
  auto& loop_ref = current_loop();
  auto allocator = current_allocator();
  auto [prom, fut] = detail::make_promise_future_impl<void>(allocator);
  auto* node = allocator.template new_object<detail::yield_resume_node>(std::move(prom));
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
yield_execution();` holds the only other reference to its
`future_state<void>`, so silently dropping the promise would strand that
coroutine's frame forever if the loop is destroyed first.

### `make_ready_future<T>(args...)`

`est::mutex::lock()`/`acquire()`'s own uncontended fast paths and
`est::counting_event<Mode>::wait()`'s own already-signaled fast path each
need an already-ready `future<T>` built from a value they already have in
hand. `make_ready_future<T>(args...)` (`est:promise`) is that pattern
factored out - `make_promise_future<T>()` followed by
`promise<T>::set_value(T(args...))`, in one call:

```cpp
template <class T, class... Args>
[[nodiscard]] auto make_ready_future(Args&&... args) -> future<T> {
  auto [prom, fut] = make_promise_future<T>();
  if constexpr (std::is_void_v<T>) {
    prom.set_value();
  } else {
    prom.set_value(T(std::forward<Args>(args)...));
  }
  return std::move(fut);
}
```

One overload covers every `T`, `void` included, since `make_promise_future<T>()`
itself now takes no explicit `loop&` to disambiguate against - there's
nothing for an `Args...` pack to be mistaken for. `mutex::lock()`'s fast
path, for instance:

```cpp
inline auto mutex::lock() -> future<void> {
  if (state_ == 0) {
    state_ = 1;
    return make_ready_future<void>();
  }
  auto allocator = current_allocator();
  auto [prom, fut] = detail::make_promise_future_impl<void>(allocator);
  auto* node = allocator.template new_object<lock_resume_node>(std::move(prom));
  waiters_.enqueue(*node);
  return std::move(fut);
}
```

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
