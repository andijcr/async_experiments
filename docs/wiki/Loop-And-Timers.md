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

`loop::enqueue_ready(detail::ready_node&)` pushes onto `ready_`, a
`loop::ready_queues` - `std::array<est::intrusive_list<detail::ready_node>,
priority_levels>`, one FIFO queue per `est::Priority` level (four: see
"Priority levels and the pluggable scheduler" below) rather than a single
list. Each per-level list is the same generic intrusive-list container
`est::counting_event<Mode>` (which `est::mutex` now builds `lock()` on top
of - issue #67) and `est::future_state<T>` each use for their own queues
(see [Architecture](Architecture.md)), templated on `detail::ready_node`
specifically so `dequeue()` already hands back a `detail::ready_node*`
directly, no cast needed. `enqueue_ready()` routes into the level
`node.priority_level` names:

```cpp
void enqueue_ready(detail::ready_node& node) noexcept {
  ready_[static_cast<std::size_t>(node.priority_level)].enqueue(node);
}
```

`run_until_idle()`/`run()` drain the whole `ready_queues` array via
`drain_ready()`, delegating the actual "which level next" decision to
`scheduler_` (a `std::function<detail::ready_node*(ready_queues&)>` - see
below). Two checks run at the top of every iteration before that:
`poll_external_if_pending()` (a registered `external_event<T>` may have
changed - see "Bridging external writers" below) and
`poll_timers_if_due()` (a pending timer's deadline may have already
passed - see "Timers" below); both only ever append a newly-ready
continuation to `ready_`, the same way `enqueue_ready()` itself does,
never run one inline ahead of whatever a level's queue already holds:

```cpp
void drain_ready() {
  for (;;) {
    poll_external_if_pending();
    poll_timers_if_due();
    auto* node = scheduler_(ready_);
    if (node == nullptr) {
      return;
    }
    run_one(*node);
    if (stop_requested_) {
      return;
    }
  }
}
```

Because the array is re-checked fresh on every iteration, a continuation
that itself completes *another* `future_state` (cascading a further
continuation onto the same ready-queue) gets picked up within the same
drain pass — an arbitrarily deep, purely synchronous chain resolves inside
one `run_until_idle()` call, not one call per link.

`run_one()` does four things around actually invoking the node:

```cpp
void run_one(detail::ready_node& node) {
  const std::unique_ptr<detail::ready_node> guard(&node); // always destroy, however this exits
  const auto priority_guard = set_priority(node.priority_level);
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
- **`priority_guard`** sets the ambient `current_priority()` to `node`'s
  own priority for the whole duration of `node.run()` - see "Priority
  levels and the pluggable scheduler" below for why.
- **Long-running-callback detection**: single-threaded means one slow
  continuation blocks everything else the loop owns — timers, other ready
  work, all of it — with nothing able to preempt it. Measuring and
  reporting a stall lives on `platform::interface` -
  `reset_loop_stall_detection()`/`detect_loop_stall(threshold)` — for the
  same reason `uptime()`/`sleep_until()` are platform hooks rather than
  `est::loop` calling `std::chrono`/`std::this_thread` directly: "how do
  we know a callback ran long" is a policy a backend should get to answer
  for itself. `est::loop` only calls the two bracketing hooks and owns the
  threshold value (currently 50ms, a starting point, not tuned against a
  real workload); both are pure virtual on `platform::interface` itself
  (see the `current_loop()` section above for why `interface` carries no
  default bodies or state of its own at all), so each concrete backend
  answers them itself. `hosted_stdcpp`'s own override just records
  `uptime()` on reset and compares against it on detect, printing via
  `platform::printdbg()` if exceeded. A future backend could implement
  both to run a watchdog on a background thread instead, catching (and
  reporting) a stall in parallel while the callback is still running,
  rather than only finding out once it returns — without `run_one()`
  itself changing at all.

### Priority levels and the pluggable scheduler

Issue #31 (four levels - `est::Priority`: `background`, `normal`, `high`,
`critical`) lets a caller ask that some ready work run before other ready
work, rather than every continuation being equally FIFO regardless of how
much it matters. Each `detail::ready_node` carries its own
`priority_level` field (a plain public member, defaulting to
`Priority::normal` - no getter/setter, matching
`intrusive_list_node::next`'s own already-public style), and
`enqueue_ready()` (above) routes purely off that field.

**Setting it explicitly**: `future<T>::then()`/`then_fast()` take an
optional trailing `Priority` argument:

```cpp
auto response = handle_event(request).then(respond, est::Priority::high);
```

**Inheriting it implicitly**: omitting that argument doesn't default to
`Priority::normal` - it defaults to `est::current_priority()`, read *at the
`then()`/`then_fast()` call site itself* (`priority prio =
current_priority()` as the parameter's own default-argument expression).
`current_priority()`/`set_priority()` (an RAII guard, modeled on
`make_current_loop()`'s own shape but a plain save/restore stack rather
than a single slot - nested priority scopes are the expected case here, not
a programming error) are a `thread_local`, exactly like
`current_loop()`/`current_allocator()`, but declared in `:loop` itself
rather than `:util.current_loop` - that partition already imports `:loop`,
so `:loop` importing back would be circular, the identical constraint that
already keeps `detail::current_allocator_new_delete<T>` a separate mixin
in `:util.current_loop` instead of living directly on `ready_node`.

This is what makes *inheritance through a chain* work automatically,
without threading a parameter through every intermediate call:
`run_one()`'s `priority_guard` (above) sets `current_priority()` to the
currently-running node's own `priority_level` for the whole duration of
its `run()` - so anything that callback goes on to register (another
`then()`, or a `co_await` - `future_awaiter<T>::await_suspend()` stamps
its own resume node's `priority_level` from `current_priority()` the same
way, since `co_await`'s syntax has no room for an extra argument the way
`then()` does) inherits the same priority by default. A caller that wants
to opt a chain *out* of inheriting (a "background" sub-chain kicked off
from otherwise high-priority work) passes `Priority::background`
explicitly, or wraps the dispatch in its own `set_priority()` scope.

**Draining**: `loop::scheduler_` is a `std::function<detail::ready_node*(
ready_queues&)>` - it pops and returns the next node to run itself
(`nullptr` once every level is empty), not just picks a level, since a
fairness-oriented policy needs to track its own per-level state to decide
that. `std::function`, not `std::move_only_function` as the issue itself
first proposed: the pinned Clang 22 libc++ snapshot this codebase builds
against doesn't implement `std::move_only_function` yet
(`__cpp_lib_move_only_function` is undefined) - a pragmatic substitute, not
a design change, since every scheduler this codebase actually needs only
closes over plain, copyable state (counters, at most). Deliberately a
type-erased callable rather than a second `platform::interface`-style
virtual base: a scheduler decision happens on the hottest path this
codebase has, once per ready node, so it stays the same kind of type
erasure `std::pmr::polymorphic_allocator` already is throughout this
codebase, not a second inheritance hierarchy.

`loop::eager_scheduler` (the default, set by `loop`'s constructor) always
drains the highest non-empty level first - simple, and matches "critical
actually means critical," at the accepted cost that sustained high-priority
traffic can starve a lower level indefinitely.

**Explicitly out of scope so far** (documented, not built): a
fairness-oriented "proportionate" scheduler alternative that bounds how
long a level can starve (issue #108); and starvation *detection* itself,
proposed to mirror `reset_loop_stall_detection()`/`detect_loop_stall()`
above exactly (per-level head-pointer-hasn't-advanced-in-N-ms, delegated
to `platform::interface` the same way - issue #109) - real, deliberate
follow-ups rather than gaps nobody noticed. `est::spawn()`
(`est/src/spawn.cppm`, issue #58) *does* take the same trailing, inheriting
`Priority prio = current_priority()` parameter `then()`/`then_fast()` do,
stamped on the completion continuation it registers - see
[Coroutines](Coroutines.md) for `spawn()` itself. The flatten/monadic path
(`detail::flatten_forwarder<T>`) was briefly in this same state - it
missed the two bypass-path fixes above - but is now fixed too (issue
#110): `fulfill()` (`est/src/future.cppm`) stamps `priority_level =
current_priority()` on the forwarder node it constructs, the same way
`yield_execution()`/`counting_event<Mode>::wait()` do.

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
  auto node = std::make_unique<detail::sleep_resume_node>(std::move(prom));
  loop_ref.schedule_timer(*node, deadline);
  node.release(); // schedule_timer() isn't noexcept - guarded until it succeeds
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
  auto node = std::make_unique<detail::promise_resume_node<void>>(std::move(prom));
  loop_ref.enqueue_ready(*node);
  node.release(); // ownership transfers to ready_
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
  auto node = std::make_unique<detail::promise_resume_node<void>>(std::move(prom));
  waiters_.enqueue(*node);
  node.release(); // ownership transfers to waiters_
  return std::move(fut);
}
```

`schedule_timer()` records the deadline in `est::timer_queue`'s own
min-heap *and* the node in `loop`'s own `pending_timers_` list, keyed by the
timer queue's own id:

```cpp
auto schedule_timer(detail::timer_node& node, clock::time_point deadline) -> timer_id {
  pending_timers_.reserve(pending_timers_.size() + 1);   // see below
  const auto id = timers_.schedule_at(deadline);
  pending_timers_.push_back(pending_entry{.id = id, .node = &node});
  return id;
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

`run_impl()`'s main loop sleeps for exactly as long as the *next* deadline
(or indefinitely, kept alive only by a registered external source - see
"`run()` vs. `run_until_idle()`" below), then loops back to `drain_ready()`:

```cpp
void run_impl(bool wait_for_external) {
  ...
  for (;;) {
    drain_ready();
    if (stop_requested_) { return; }
    const auto deadline = timers_.next_deadline();
    if (!deadline && (!wait_for_external || external_sources_.empty())) {
      return; // nothing ready, nothing pending, nothing worth waiting on
    }
    platform::instance().interruptible_sleep_until(deadline.value_or(clock::time_point::max()));
  }
}
```

`platform::instance().interruptible_sleep_until()` is why a test doesn't
have to actually wait real wall-clock time for a timer-driven test to
complete: a fake platform overrides it to advance its own fake clock
instantly instead of blocking (mirroring the same seam `uptime()`/
`assert_failure()` already use — see `est/tests/loop_tests.cpp`'s
`fake_platform`). There's no explicit `fire_ready_timers()` call here any
more, unlike an earlier version of this loop: `drain_ready()`'s own
`poll_timers_if_due()` (above) is unconditionally the first thing its
loop does on the very next iteration, so whatever just woke this sleep
gets picked up there instead - one call site for firing timers, not two.
`fire_ready_timers()` itself is unchanged: it pops every timer whose
deadline has passed, looks it up in `pending_timers_` to find its node,
and calls `fire()` — which for a `sleep_for()`-created node just does
`prom.set_value()`, which in turn triggers `future_state<void>::
complete()`, which enqueues *its* continuations onto the same ready-queue
`drain_ready()`'s own loop is already mid-iteration on, so they're picked
up without waiting for a separate pass.

Because `poll_timers_if_due()` runs every `drain_ready()` iteration, not
just once `ready_` finally empties, a due timer no longer waits behind an
arbitrarily long (or self-replenishing) chain of ready-queue work the way
it used to: a continuation that keeps re-enqueuing more ready work used
to starve `fire_ready_timers()` of ever running at all, since it was only
ever reached *after* `drain_ready()` returned. Firing a timer mid-drain
only ever appends its continuation to `ready_`, exactly like an external
source's own `poll()` call already does - it never preempts whatever that
priority level's queue already holds, just joins the back of it.

### `cancel_timer()`: pulling a still-pending registration out early

`schedule_timer()`'s returned `timer_id` (`timer_queue<allocator_type>::id`)
is what a caller keeps if it might need to cancel that specific
registration later - most callers (`sleep_until()` with no `stop_token`,
`schedule_periodic()`'s own re-arming) simply discard it. `cancel_timer()`
does one iteration of what `drain_pending()` already does for *every*
pending timer at teardown, on demand, for exactly one:

```cpp
[[nodiscard]] auto cancel_timer(timer_id id) noexcept -> bool {
  const auto it = std::ranges::find(pending_timers_, id, &pending_entry::id);
  if (it == pending_timers_.end()) {
    return false;   // already fired, or stale
  }
  timers_.cancel(id);
  auto* node = it->node;
  pending_timers_.erase(it);
  detail::abandon_timer_node(*node);
  return true;
}
```

Returns `false`, a no-op, if `id` no longer names a pending entry - it may
already have fired (and been erased by `fire_ready_timers()`) by the time
a caller gets around to cancelling it; a single-threaded race a
token-driven canceller can't rule out ahead of time. `timers_.cancel(id)`
itself already existed and already worked (used internally by
`drain_pending()`'s own teardown loop above) - this just exposes the same
capability for one entry, on demand, instead of only at loop destruction.
`detail::abandon_timer_node()` completes the node exactly like teardown
does (`sleep_resume_node::abandon()` sets its promise's exception to
`abandoned_exception`), so a plain `loop.cancel_timer(id)` alone always
surfaces as an abandoned wait - `est::sleep_for(delay, stop_token)`/
`est::sleep_until(deadline, stop_token)` (`est:with_stop`) build on this to
surface `est::operation_cancelled` instead, by racing the caller-visible
future against `token.stopped()` rather than exposing `sleep_resume_node`'s
own promise directly - see
[Coroutines](Coroutines.md#cancellation-stop_token-vs-abandonment).

## Periodic timers: `schedule_periodic()`

`est::schedule_periodic(interval, fn, max_jitter = {})` (`est:timer.periodic`)
calls `fn()` repeatedly, once every `interval` (plus a fresh, uniformly
distributed jitter offset each period - `est::jitter`, `:util.jitter`,
below). `Fn` must satisfy `std::invocable<Fn&>` (checked at the type, on
both `schedule_periodic()` and `detail::periodic_timer_node<Fn>` itself -
the same place `est::scope_exit`, `est:util.scope_exit`, constrains its
own stored `Fn`) - `Fn&`, not plain `Fn`, since `fn_` is invoked
repeatedly as a named member, not just once. Built entirely on
`schedule_timer()` above, with no change to `loop.cppm` itself:
`loop::fire_ready_timers()` unconditionally deletes every `timer_node`
right after `fire()` returns - the same one-shot contract
`sleep_resume_node` already relies on - so a periodic timer can't reuse
itself in place. Instead, `detail::periodic_timer_node<Fn>::fire()` hands
off to a **fresh** node for the next period before returning:

```cpp
void fire() override {
  if (ctrl_->cancelled) { return; }
  const auto period_start = platform::instance().uptime(); // before fn_(), not after
  try {
    fn_();
  } catch (...) {
    platform::printdbg("...");   // loud, but doesn't stop the loop or the chain
  }
  if (ctrl_->cancelled) { return; }   // fn_ itself may have just cancelled
  auto& loop_ref = current_loop();    // resolved fresh, same as every other node
  const auto offset = jitter_();
  auto next = std::make_unique<periodic_timer_node>(std::move(fn_), interval_, jitter_, ctrl_);
  loop_ref.schedule_timer(*next, period_start + interval_ + offset);
  next.release(); // schedule_timer() isn't noexcept - guarded until it succeeds
}
```

**Fixed-rate, not fixed-delay.** `period_start` is captured *before*
`fn_()` runs, and the next deadline is computed from it, not from a
`uptime()` read taken after `fn_()` returns - so a slow or variable-latency
`fn_()` doesn't push every later period further out by however long that
call happened to take (the classic fixed-delay drift a naive
`setTimeout()`-chain has). This is what a "periodic timer" means in
practice: a `setInterval()`-style fixed cadence, anchored to when each
period *started*, not to how long the previous one's work took.

**A throwing `fn_()` is caught, not left to propagate.** `fn_` has no
downstream `future<T>` to route an exception into the way `.then()`
continuations do (`concrete_continuation<Fn, U>::run()`,
[Continuation Node Mechanism](Continuation-Node-Mechanism.md)) - it
returns `void`. Left uncaught, an exception would unwind
`loop::fire_ready_timers()`/`run_impl()` entirely, abandoning every other
unrelated pending timer and ready-work item on the same loop over one
callback's own bug, and silently killing the chain forever. Caught and
reported via `platform::printdbg()` instead (the same "loud diagnostic,
keep going" tool `loop::run_one()`'s own long-running-callback stall
detection already uses) - the period is skipped, the chain still
reschedules.

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
answers it via `std::random_device`, falling back to `uptime()`'s own bit
pattern if that throws; a future bare-metal backend would answer from
whatever hardware entropy source it has). Not cryptographically secure,
nor does it need to be - jitter only has to differ from the last draw,
never resist prediction.

## Bridging external writers: `est::external_event<T>` (`:sync.external_event`)

`est::schedule_periodic()` above answers "run this periodically"; issue
#74 asked for the other half - "wake up when something *outside* this
loop's own call stack changes." `est::external_event<T>` bridges a value
written from another thread, an ISR, or (on a future bare-metal target) a
hardware register into `est`'s existing cooperative event system
(`est::binary_event<EventResetMode::manual>`, above) - the *one*
deliberate exception to this codebase's single-threaded/no-atomics rule
(`CLAUDE.md`): the `std::atomic<T>&` it wraps is the FFI boundary itself,
never touched anywhere in this class except inside `poll()`.

```cpp
template <class T>
  requires std::equality_comparable<T>
class external_event : private detail::external_source {
public:
  explicit external_event(std::atomic<T>& source) noexcept;
  external_event(std::atomic<T>& source, loop& owner);   // opt-in loop registration - see below

  void poll() noexcept override;               // loop-thread only, never suspends
  [[nodiscard]] auto wait() -> future<void>;    // delegates to the internal binary_event
  void reset() noexcept;                        // re-arms for the next change
  [[nodiscard]] auto value() const noexcept -> T;
  [[nodiscard]] auto notifier() const noexcept -> external_notifier;   // loop-registered instances only

private:
  std::atomic<T>* source_;
  T last_seen_;
  binary_event<EventResetMode::manual> event_;
  loop* owner_ = nullptr;
};
```

`T` must be lock-free (`static_assert(std::atomic<T>::is_always_lock_free)`)
- `poll()` is meant to be callable from a context as constrained as a
periodic timer callback (eventually: an ISR), so it must never silently
block on a fallback lock the way a non-lock-free `std::atomic<T>` could.
`poll()` is the *only* place this class ever reads `source_`: it loads
with `memory_order_acquire` (pairing with the external writer's own
`memory_order_release` store or stronger) and, if the value changed since
the last poll, updates `last_seen_` and calls `event_.set()`. Everything
else - the cached `last_seen_`, the internal `binary_event` - lives
entirely on the loop thread, same single-threaded assumption as
everywhere else in `est`.

**Level-triggered, with an explicit `reset()` step, not edge-triggered
per change.** `poll()` calling `event_.set()` (idempotent, stays signaled
until `reset()`) rather than something that fires once per distinct value
means a consumer that calls `wait()` after several changes have already
happened still resolves immediately via `binary_event`'s own
already-signaled fast path, observing the *latest* value via `value()` -
it was never meant to replay every intermediate one. A change that
happens before the consumer calls `reset()` is folded into the value
`reset()` will next observe, not lost or double-counted; a change *after*
`reset()` sets the event again. This mirrors every other `EventResetMode::
manual` primitive in this codebase (`counting_event<manual>`,
`binary_event<manual>`) rather than inventing new semantics.

**Two ways to drive `poll()`.** A caller can still wire it into
`schedule_periodic()` explicitly, the original (issue #74) design:

```cpp
std::atomic<int> reading{0};             // written by another thread/ISR
est::external_event<int> bridge{reading};
auto handle = est::schedule_periodic(50ms, [&bridge] { bridge.poll(); });
// ... co_await bridge.wait(); use bridge.value(); bridge.reset();
```

This lets one periodic timer's callback drive several sources' `poll()`
calls at once, or drive `poll()` from something other than a timer
entirely, and keeps `external_event<T>` trivially unit-testable without
any real second thread: a test just assigns directly to the
`std::atomic<T>` it constructs the bridge over, single-threaded, matching
every other test in this codebase (`est/tests/external_event_tests.cpp`).
It also pays a real, up-to-one-poll-interval latency tax and needs an
already-running (or newly-added) periodic timer to bridge with.

The second constructor (issue #125) removes that tax by registering
directly with a `loop`, letting it call `poll()` at its own dispatch
checkpoints instead:

```cpp
std::atomic<int> reading{0};
est::external_event<int> bridge{reading, loop};
auto notifier = bridge.notifier();       // handed to the external writer

// external context (an ISR, or a real std::jthread):
reading.store(new_value, std::memory_order_release);
notifier.notify();

// loop thread, as before:
co_await bridge.wait();
```

`bridge` implements `detail::external_source` (`est:loop`) for exactly
this purpose - `register_external()`/`unregister_external()` (called
from the constructor/destructor) add and remove a raw observer pointer
into `loop`'s own registry, and `est::loop` never allocates or owns it
(the same reasoning `est::mutex`/`est::counting_event<Mode>`'s own
"loop must outlive me" precondition already has elsewhere - see this
page's own "structural hazard" section above). `notifier()` fails a
`check()` on an instance built with the base constructor - there's no
`loop&` to hand a notifier a reference to.

`external_notifier` is a tiny, copyable handle safe to call from any
context that can call anything at all - an ISR, a real second thread.
`notify()` forwards straight to `loop::notify_external()`, which itself
does two things: the `std::atomic<bool>` store (see "The shared
pending-external flag" below) and `platform::instance().wake(id_)`,
resolved fresh *wherever `notify_external()` actually runs* -
deliberately not through a pointer captured back when `notifier()` was
constructed. (Those two calls used to live split across
`external_notifier::notify()` and `loop::notify_external()`
separately - consolidated into the one method once it became clear
`notify_external()` had exactly one caller and gained nothing from the
split.) That thread-local-dispatch distinction still matters the same
way: `platform::instance()` is `thread_local`
([Global Lookup Codegen](Global-Lookup-Codegen.md)), so a real producer
thread calling `notify()` needs its *own* `platform::interface`
installed first (typically the same backend object the loop thread
itself uses, via `platform::override_instance()`) - the same
requirement any thread driving an `est::loop` already has. An ISR
doesn't need this on a single-core bare-metal target: it shares the
loop thread's own TLS block by construction, confirmed empirically
against `estmsp`.

### The shared pending-external flag, and `interruptible_sleep_until()`

`est::loop` checks *one* `std::atomic<bool>` (`pending_external_`) at the
top of every `drain_ready()` iteration - cheap enough to do unconditionally,
whether or not anything is registered. Only when that flag is observed set
does it walk the registered-source list and call `poll()` on each one
(`loop::poll_external_if_pending()`). This is deliberately coarser than
"which source changed" - a false wake just costs each *other* registered
source one wasted `poll()` call, negligible against the alternative (a
per-source dirty bit, or polling every source on every single ready node
regardless of whether the flag is set).

This one check site covers both of what used to be two separate
concerns: draining the ready-queue while the loop is busy, and waking up
from an idle block. `run_impl()`'s own `for (;;)` loop calls
`drain_ready()` immediately after `platform::instance().interruptible_sleep_until()`
returns, so an external wake that arrived while the loop was blocked is
picked up there without a second, separate call - the same checkpoint
also covers a due timer now (`poll_timers_if_due()`, "Timers" above),
so `drain_ready()` is the one place both get noticed, whether the loop
just woke from a real sleep or is mid-drain of a long ready-queue chain.

`platform::interface::sleep_until()` was renamed to
`interruptible_sleep_until()` as part of this design, with a changed
contract: it *may* return before `deadline`, for any reason including
none - a caller must always re-check `uptime()` against `deadline`, never
assume it passed just because the call returned. `platform::interface::
wake(WakeId)`/`wake_all()` are the paired hooks a backend implements to
make early return meaningful: `hosted_stdcpp` uses a real
`condition_variable`, `estwasm` reuses the same `Atomics.wait()`/
`Atomics.notify()` primitive it already had, and `estmsp` needs nothing
extra at all - a real hardware interrupt already unblocks a bare-metal
`wfi` for free, so its `wake()`/`wake_all()` are genuine no-ops. `WakeId`
is an opaque, backend-defined value (a core index on a future multi-core
bare-metal target; irrelevant on every backend that exists today, each
of which only ever drives one loop per instance) - see `docs/PLAN.md`'s
issue #125 entry for the full reasoning, including why an earlier
version of this design routed `wake()` through a captured pointer
instead, and why that didn't generalize.

## A stream instead of a value: `est::spsc_ring<T>` (`:sync.spsc_ring`)

`est::external_event<T>` above is level-triggered on a single value - a
write between two `poll()` calls that gets overwritten again before the
next one is silently lost, the right tradeoff for a sensor reading or a
status flag. `est::spsc_ring<T>` (issue #96) answers the other half:
every value pushed reaches the consumer exactly once, in order, or is
rejected outright (never silently dropped) - for a stream of discrete
items (log records, incoming frames, queued commands) where losing one
isn't acceptable.

```cpp
template <class T>
  requires std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
           std::default_initializable<T>
class spsc_ring {
public:
  explicit spsc_ring(std::size_t capacity, allocator_type allocator = {});

  [[nodiscard]] auto try_push(T&& value) noexcept -> bool;     // external-context only
  [[nodiscard]] auto try_pop() noexcept -> std::optional<T>;   // loop-context only
};
```

Exactly one producer (external context) ever calls `try_push()`, exactly
one consumer (loop context) ever calls `try_pop()` - the same "the atomic
is the one deliberate FFI boundary, everything else stays single-
threaded" shape `external_event<T>` already established for one value,
generalized to a bounded stream.

**`capacity() + 1` slots, not `capacity()`** - the classic ring-buffer
trick for telling full apart from empty using nothing but the two
indices already needed anyway, no separate atomic count or generation
bit:

```cpp
[[nodiscard]] auto try_push(T&& value) noexcept -> bool {
  const auto write_index = write_index_.load(std::memory_order_relaxed);
  const auto next_write = advance(write_index);
  if (next_write == read_index_.load(std::memory_order_acquire)) {
    return false; // full - value untouched, still owned by the caller
  }
  buffer_[write_index] = std::move(value);
  write_index_.store(next_write, std::memory_order_release);
  return true;
}
```

Takes `T&&`, not `T` by value: the full check has to run *before* `value`
is touched at all, not just before it's written into `buffer_`. A
by-value parameter would already have moved out of the caller's object at
the call site (`try_push(std::move(x))` moves into the parameter
unconditionally, whether or not the push then succeeds), so a rejected
push of a move-only `T` would silently destroy it with no way to hand it
back - exactly the "this data actually matters" guarantee reject-on-full
exists to uphold. Binding by reference instead defers the move to the one
`std::move(value)` above, which runs only once the ring is known to have
room.

`write_index_` is read with `memory_order_relaxed` here for the same
reason `external_event<T>::poll()` needs no ordering at all to read its
own `last_seen_`: `write_index_` is written *only* by `try_push()`
itself, so this call's own prior write is already visible to its own
later read via plain program order - no cross-thread synchronization
needed for a value nothing else ever writes.

**Two atomics, two producer/consumer roles, two acquire/release
pairings** - more involved than `external_event<T>`'s single atomic, so
spelled out explicitly rather than left implicit:

- `try_push()`'s `read_index_.load(acquire)` pairs with `try_pop()`'s own
  `read_index_.store(release)` (below) - not because `try_push()` reads
  *through* `read_index_`, but so "the ring is full" reflects every slot
  `try_pop()` has actually finished reading, never a stale view that
  would reject a push the consumer already made room for.
- `try_push()`'s `write_index_.store(release)` pairs with `try_pop()`'s
  own `write_index_.load(acquire)` - this is the pairing that actually
  publishes the value: the release store doesn't just make the new index
  visible, it makes everything sequenced before it (the plain,
  unsynchronized write into `buffer_[write_index]` just above) visible to
  whichever `try_pop()` call's acquire load first observes the new index.

```cpp
[[nodiscard]] auto try_pop() noexcept -> std::optional<T> {
  const auto read_index = read_index_.load(std::memory_order_relaxed);
  if (read_index == write_index_.load(std::memory_order_acquire)) {
    return std::nullopt; // empty
  }
  std::optional<T> value{std::move(buffer_[read_index])};
  read_index_.store(advance(read_index), std::memory_order_release);
  return value;
}
```

The two functions are exact mirror images: `try_pop()`'s own
`read_index_.load(relaxed)` needs no ordering for the identical reason
`try_push()`'s `write_index_.load(relaxed)` doesn't (only `try_pop()`
itself ever writes `read_index_`), and its `write_index_.load(acquire)`/
`read_index_.store(release)` pair with `try_push()`'s own release/acquire
the same way, just with producer and consumer swapped.

**`T` constrained to nothrow-movable and default-constructible** - move,
not copy: `try_push()`/`try_pop()` move `T` into and out of `buffer_`
rather than copy it, so a move-only slot type works, most usefully
`est::spsc_ring<std::unique_ptr<U>>` handing off ownership of a
heap-allocated item per slot, alongside plain PODs (an `int`, a small
struct, a `std::chrono` duration). The moves must be `noexcept` - a
throwing move out of `buffer_` would leave a slot's state ambiguous
mid-handoff, exactly the hazard `try_push()`/`try_pop()`'s own `noexcept`
is meant to rule out (also why the `NOLINTNEXTLINE(cppcoreguidelines-pro-
bounds-avoid-unchecked-container-access)`-marked accesses in
`est/src/sync/spsc_ring.cppm` stay unchecked: a bounds-checked `.at()`
would reintroduce exactly that exception, for an index that's provably
always in range by construction). A trivially copyable `T` satisfies this
trivially - its "move" is the same non-throwing copy it always was - so
this is a strict relaxation of the class's original trivially-copyable-
only constraint, not a different shape.

**Composition with `external_event<T>`/`schedule_periodic()`, not
inheritance or reuse** - the ring buffer and the periodic timer solve
different problems and neither is built in terms of the other; a caller
wires them together explicitly, the same way `external_event<T>::poll()`
above is wired into `schedule_periodic()`:

```cpp
est::spsc_ring<int> queue(64);                  // written by another thread/ISR
auto handle = est::schedule_periodic(10ms, [&queue] {
  while (const auto item = queue.try_pop()) {
    // ... handle *item ...
  }
});
```

The simplest drain strategy - unconditionally draining to empty every
period, shown above - always pays the drain-loop cost even when nothing
was pushed since the last period. A caller with a genuinely idle-most-
of-the-time queue could instead layer `est::external_event<std::size_t>`
over a monotonic write-counter bumped alongside every successful
`try_push()`, only draining when that counter actually moved - a real
optimization for that shape, but not something `spsc_ring<T>` itself
needs to know about either way.

Every test in `est/tests/spsc_ring_tests.cpp` simulates the producer by
calling `try_push()` directly from the test body, single-threaded,
matching this codebase's established convention
(`external_event_tests.cpp`) - `spsc_ring<T>`'s own contract only
requires `try_push()`/`try_pop()` never run concurrently with themselves,
not that they run on genuinely different threads to be exercised
correctly. That's also what lets this file (unlike the two below) build
and run on `estmsp`'s bare-metal `mps2an385` target, which has no
`<thread>` at all.

Real concurrent access gets its own, separate coverage instead, one
file per execution-context shape: `est/tests/spsc_ring_thread_tests.cpp`
has a real `std::jthread` producer racing a `schedule_periodic()`-driven
consumer on the loop thread, using the real `platform::interface` (a
real clock, a real blocking `sleep_until()`) rather than a fake one, so
the two threads actually interleave over wall-clock time instead of the
fake clock's instantaneous `sleep_until()` starving the producer of any
window to run in - only buildable where `<thread>` exists, so excluded
from `mps2an385`. That target gets its own analogous coverage instead:
`examples/multicolor_larson_scanner/mps2an385/tests/spsc_ring_isr_tests.cpp`
races a real hardware interrupt (self-triggered via the NVIC's own
Interrupt Set-Pending Register, not a real peripheral) against a
mainline producer - the actual cross-context hazard this backend faces
in production (`estmsp::enqueue_output()`'s mainline producer racing
`UART0_TX_Handler`'s ISR consumer, above). `spsc_ring<T>` is the one
type in this codebase whose whole contract is a real cross-context
handoff, so on every target that can express one at all, it earns a
test that actually crosses it.

## `run()` vs. `run_until_idle()`, and `stop()`

Both drain ready work and sleep until the next deadline, repeat, until
`stop()` is called — but they now genuinely diverge on when "idle" ends
the loop, because a registered `external_event<T>` (see "Bridging
external writers" above) gives a loop a real reason to stay alive with
no timer pending at all. `run_impl()` takes a `bool wait_for_external`:
`run_until_idle()` passes `false` — its contract stays "return as soon
as there's nothing to do *right now*," unaffected by any registered
source that just hasn't fired yet, matching every existing caller's
expectations. `run()` passes `true` — it now only returns when there is
neither a pending timer *nor* any registered external source at all
(`external_sources_.empty()`); with a source registered, `run()` blocks
on `interruptible_sleep_until(clock::time_point::max())` (interrupted
only by `wake()`/`wake_all()`, or a harmless backend-specific spurious
wake) rather than returning immediately. This is the real difference the
two names always implied; it just needed a genuine wakeup source other
than a timer to become observable, and `external_event<T>`'s loop
registration is the first thing in this codebase that provides one.

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
