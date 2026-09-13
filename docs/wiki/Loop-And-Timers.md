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
`yield_execution()` (all `est:promise`), `est::counting_event<Mode>::set()`
(and, through it, `est::mutex::unlock()`), and a coroutine's own
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
`abandon()`, which calls the coroutine handle's own `destroy()`, which
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
container `est::counting_event<Mode>` (which `est::mutex` now builds
`lock()` on top of - issue #67) and `est::future_state<T>` each use for
their own queues (see [Architecture](Architecture.md)), templated here on
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
  const std::unique_ptr<detail::ready_node> guard(&node); // always destroy, however this exits
  platform::instance().reset_loop_stall_detection();
  node.run();
  platform::instance().detect_loop_stall(long_running_threshold);
}
```

- **`guard`** (a plain `std::unique_ptr<detail::ready_node>`, holding the
  same reference `node` names - `fire_ready_timers()` below has the
  identical one-liner for `detail::timer_node`) guarantees the node is
  deallocated no matter how `run()` returns — even though a continuation
  node's own `run()` already catches every exception a callback could
  throw internally, so this is a defensive guarantee, not something the
  happy path relies on. Not factored into a shared helper (an earlier
  version of this code had one, `destroy_guard<Node>()`): once its body
  shrank to exactly `unique_ptr<Node>(&node)`, the template stopped
  earning its keep over writing the same one line at each of the two
  call sites.
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
  auto [prom, fut] = detail::make_promise_future_impl<void>(current_allocator());
  auto* node = new detail::sleep_resume_node(std::move(prom));
  loop_ref.schedule_timer(*node, deadline);
  return std::move(fut);
}
```

`current_loop()` is still resolved here (`schedule_timer()` needs the
loop itself); the node's own allocation instead resolves
`current_allocator()` internally, inside its own `operator new` - the
same "reach the allocator without a detour through the loop pointer"
pattern used everywhere current_loop() isn't independently needed.

`sleep_resume_node` holds the `promise<void>` directly rather than
wrapping a generic closure - needed so `abandon()` can complete the
promise with an exception when the timer never fired before the loop was
destroyed (`abandon()` is only ever called on that path - see
[Continuation Node Mechanism](Continuation-Node-Mechanism.md)). A
generic, type-erased callback would give `abandon()` no way to know it's
holding a promise at all, and a silent drop instead would have exactly
the stranded-coroutine-frame hazard described in
[Coroutines](Coroutines.md)'s `promise_resume_node<T>` section.

`yield_execution()` gives `current_loop()` the chance to run
whatever else is already ready before the calling coroutine resumes. It
has no real deadline to track, so it's a direct
`detail::promise_resume_node<void>` handed straight to
`loop_ref.enqueue_ready()` - a plain `ready_node` holding a `promise<void>`,
no timer machinery at all, and the same shared node type
`counting_event<Mode>::wait()`'s own slow path uses below (issue #77
collapsed what used to be two separate, byte-identical node types into
this one):

```cpp
[[nodiscard]] inline auto yield_execution() -> future<void> {
  auto& loop_ref = current_loop();
  auto [prom, fut] = detail::make_promise_future_impl<void>(current_allocator());
  auto* node = new detail::promise_resume_node<void>(std::move(prom));
  loop_ref.enqueue_ready(*node);
  return std::move(fut);
}
```

FIFO is what makes this safe: `enqueue_ready()` appends at the tail, so
everything already queued when `yield_execution()` is called runs first
(see [Architecture](Architecture.md) and `intrusive_list<T>`'s own doc
comment for why the list is FIFO). `promise_resume_node<T>::abandon()`
completes its promise with `detail::abandoned_exception` (`est:loop`)
rather than silently dropping it, for the same reason its own doc
comment gives (`docs/wiki/Coroutines.md`) - a coroutine suspended via
`co_await yield_execution();` holds the only other reference to its
`future_state<void>`, so silently dropping the promise would strand that
coroutine's frame forever if the loop is destroyed first.

### `make_ready_future<T>(args...)`

`est::counting_event<Mode>::wait()`'s own already-signaled fast path
(which `est::mutex::lock()`'s own fast path builds on top of via
`try_wait()` - see [Coroutines](Coroutines.md)) needs an already-ready
`future<T>` built from a value it already has in hand.
`make_ready_future<T>(args...)` (`est:promise`) is that pattern factored
out - `make_promise_future<T>()` followed by
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
nothing for an `Args...` pack to be mistaken for. `counting_event<Mode>::
wait()`'s fast path, for instance:

```cpp
[[nodiscard]] auto wait() -> future<void> {
  if (try_wait()) {
    return make_ready_future<void>();
  }
  auto [prom, fut] = detail::make_promise_future_impl<void>(current_allocator());
  auto* node = new detail::promise_resume_node<void>(std::move(prom));
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

## Periodic timers: `schedule_periodic()`

`est::schedule_periodic(interval, fn, max_jitter = {})` (`est:timer.periodic`)
calls `fn()` repeatedly, once every `interval` (plus a fresh, uniformly
distributed jitter offset each period - `est::jitter`, `:util.jitter`,
below). Built entirely on `schedule_timer()` above, with no change to
`loop.cppm` itself: `loop::fire_ready_timers()` unconditionally deletes
every `timer_node` right after `fire()` returns - the same one-shot
contract `sleep_resume_node` already relies on - so a periodic timer can't
reuse itself in place. Instead, `detail::periodic_timer_node<Fn>::fire()`
hands off to a **fresh** node for the next period before returning:

```cpp
void fire() override {
  if (ctrl_->cancelled) { return; }
  fn_();
  if (ctrl_->cancelled) { return; }   // fn_ itself may have just cancelled
  auto& loop_ref = current_loop();    // resolved fresh, same as every other node
  const auto offset = jitter_();
  auto* next = new periodic_timer_node(std::move(fn_), interval_, jitter_, ctrl_);
  loop_ref.schedule_timer(*next, platform::instance().now() + interval_ + offset);
}
```

`fn_`/`jitter_` are moved/copied forward into each successive node rather
than re-created, so the same jitter PRNG state (and the same callable)
carries across the whole chain - only the node's own allocation is fresh
each period, the same way every other node type in this codebase is a
one-shot, freshly-allocated object rather than something reused in place.

**Cancellation** is a small shared `detail::periodic_timer_control{bool
cancelled}`, referenced by every node in the chain (`est::shared_ptr`) and
by the `periodic_timer_handle` `schedule_periodic()` returns to the
caller. `handle.cancel()` (or `fn_` itself calling it) just flips the
flag - checked at the top of `fire()` (stops even an already-scheduled
but not-yet-fired node from calling `fn_()`) and again after `fn_()`
returns (stops `fn_` from being able to reschedule itself one last time
after cancelling). Not atomic: this is loop-thread-side state, the same
single-threaded assumption as everywhere else in `est`.

`interval` must be positive and `max_jitter` strictly less than `interval`
(both `est::check()`-enforced): a jittered delay of exactly zero risks the
rescheduled node landing in the very timer batch that's still firing
(`fire_ready_timers()` evaluates "now" once per batch, so a same-instant
reschedule would be picked up and re-fired before that batch ever returns
to `loop`'s own outer `for (;;)`), and jitter is meant to perturb a
period, not invert or collapse it.

### `est::jitter` (`:util.jitter`)

A small, self-seeding uniform-jitter generator - `std::minstd_rand` (a
single-word-state 32-bit LCG, not `std::mt19937`'s much larger state) plus
a `std::uniform_int_distribution` over `[-max_jitter, +max_jitter]`.
Seeded once, at construction, from `platform::instance().get_random_seed()`
(`:platform`) - the one place in this codebase that needs actual
randomness, and the one new platform hook this feature added (`hosted_stdcpp`
answers it via `std::random_device`, falling back to `now()`'s own bit
pattern if that throws; a future bare-metal backend would answer from
whatever hardware entropy source it has). Not cryptographically secure,
nor does it need to be - jitter only has to differ from the last draw,
never resist prediction.

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
