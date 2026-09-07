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

## `loop::current()`: opt-in, not automatic, and not `thread_local` (issue #30)

A caller can still avoid threading a `loop&` through by hand, without
reopening the global-singleton problem the section above rules out -
`loop::make_current()` registers *this* loop as the one
`make_promise_future()`, `sleep_for()`/`sleep_until()`/`yield_execution()`,
and a loop-less coroutine's own `promise_type` (all consuming
`loop::current()`) fall back to, until the returned guard is destroyed:

```cpp
[[nodiscard]] auto make_current() noexcept {
  check(!detail::loop_is_current, "est::loop::make_current(): another loop is already current...");
  detail::loop_is_current = true;
  platform::instance().set_current_loop_context(this);
  return scope_exit([]() noexcept {
    detail::loop_is_current = false;
    platform::instance().set_current_loop_context(nullptr);
  });
}

[[nodiscard]] static auto current() -> loop& {
  auto* const context = platform::instance().get_current_loop_context();
  check(context != nullptr, "est::loop::current(): no loop is current (issue #30) ...");
  return *static_cast<loop*>(context);
}
```

This took four revisions to land on. The issue's own original suggestion
was a method on `est::platform` - `platform::get_loop()`.

1. The first version rejected any platform involvement at all, in favor of
   a `thread_local loop*` scoped to `loop`'s own constructor/destructor,
   reasoning that `platform::interface` is safe to make a global
   specifically *because* it's stateless policy (the "why an explicit
   `loop&`" section above), and a mutable `loop*` living there would
   reintroduce exactly the state that section rules out. That reasoning
   wasn't wrong, but it solved a problem this codebase doesn't actually
   have while creating a new one: this codebase already has a plain,
   *non*-`thread_local` mechanism doing exactly this kind of "which one is
   current" job - `platform`'s own `current_instance`/`instance()` - and
   doing the same thing for `loop::current()` costs nothing that
   global-swap pattern doesn't already pay for `platform::instance()`
   itself. `thread_local`, on the other hand, is something this codebase
   has never otherwise needed, and the repo owner's own bare-metal
   stretch goal (`docs/PLAN.md` - freestanding, no OS) is exactly the kind
   of target where `thread_local` may have no well-defined support at all.
2. Moved the slot from a second free-standing global living alongside
   `current_instance` in `:platform`, onto `platform::interface` itself as
   a plain (still not `thread_local`) data member, with
   `get_current_loop_context()`/`set_current_loop_context()` as ordinary
   (non-virtual) methods on it - per repo owner review, this belongs on
   the platform instance rather than as parallel global state next to it.
3. Went further still: `platform::interface` should be a pure interface,
   full stop - every method pure virtual, no data members of its own at
   all. The two methods that used to have a shared default body backed by
   a member declared directly on `interface` - the pair above, and
   `reset_loop_stall_detection()`/`detect_loop_stall()` (the long-running-
   callback detector, already on `interface` before issue #30) - moved
   their state and logic onto `hosted_stdcpp`. Every test fake in
   `est/tests/` deriving from `interface` needed matching overrides of its
   own from this point on - most are no-ops, but `loop_tests.cpp`'s
   `fake_platform`/`jumping_platform` need working ones, since `est::loop`
   is genuinely constructed under both.
4. **The constructor/destructor auto-registration itself got reverted -
   per repo owner review, "creating a loop and setting it as current
   should be the caller's responsibility," not something every loop
   constructor decides unconditionally.** `make_current()` above is the
   result: an explicit, RAII-scoped opt-in a caller reaches for only when
   it actually wants a specific loop to be the implicit one, modeled
   directly on `platform::override_instance()`'s own shape. Most loops in
   this codebase's own tests were never meant to be "the" current loop at
   all - forcing every one of them through the same nesting-checked slot
   was the wrong default. The nesting precondition itself moved with it,
   tracked by a dedicated `detail::loop_is_current` flag in `:loop` rather
   than by asking `get_current_loop_context()` for a null/non-null answer
   (see point 5 below for why that specific question changed meaning).
5. **This same review also asked for the ergonomic case issue #30 was
   actually chasing: a caller that never wants to think about `est::loop`
   at all.** Rather than `loop::current()` simply failing its own
   precondition until *something* calls `make_current()`,
   `hosted_stdcpp`'s own `get_current_loop_context()` now falls back to a
   loop of its own, lazily constructed on first use, whenever nothing has
   been explicitly registered - genuinely usable, not just non-null: a
   caller drives it the same way any other loop
   (`est::loop::current().run_until_idle();`), without ever writing
   `est::loop loop;` themselves. Making this possible needed one more
   change: `hosted_stdcpp` moved into its own module,
   `:platform.hosted_stdcpp`, since it now has to name `est::loop` to
   construct that fallback, and `:platform` sits below `:loop` in the
   dependency DAG specifically so it never has to (this file's own top
   comment; [Architecture](Architecture.md) has the fuller module-DAG
   picture). `est/src/est.cppm` is what actually installs `hosted_stdcpp`
   as the process's permanent default now, via the same
   `override_instance()` every test already uses to install a *temporary*
   one - just never letting the returned guard go out of scope. (This
   revisits - with a real payoff this time - a module split, issue #6,
   this codebase once closed as "no concrete payoff with only one
   backend.")

The actual storage is still a plain `void*`, not `loop*`: `:platform`
itself still can't name `est::loop` directly, even though the concrete
backend that implements `get_current_loop_context()` now can (point 5
above). Whichever backend implements it performs the cast - safe by
construction, not by RTTI, since the only call sites that ever write into
it are `make_current()`'s own guard and, for `hosted_stdcpp` specifically,
its own lazily-constructed fallback loop. `get_current_loop_context()`/
`set_current_loop_context()` are virtual (point 3 above) - not because
"hold a pointer and hand it back" is backend-specific (it still isn't),
but because `interface` no longer special-cases *any* method as
non-overridable state-holding, now that it holds no state for any of
them; a bare-metal backend might reasonably want to answer this
differently too (a fixed static slot, say, with no global initialization
order to worry about).

A single slot with a checked precondition against nesting, not a
push/pop stack like `platform::override_instance()`'s: two loops both
current at once (one calling `make_current()` while another's guard is
still alive) is treated as a programming error (an `est::check()` failure
- see `est/tests/check_tests.cpp`'s own doc comment on why that failure
path isn't unit-tested here), not "the inner one temporarily shadows the
outer."

Every free function that takes an explicit `loop&` today gained a
matching overload that pulls `loop::current()` instead -
`make_promise_future<T>()`, `sleep_for()`/`sleep_until()`,
`yield_execution()` (all `est:promise`) - and `est::mutex` gained a
matching no-argument constructor. A coroutine returning `est::future<T>`
can drop the `est::loop&` parameter the same way - see
[Coroutines](Coroutines.md)'s own calling-convention section for how
`promise_type` resolves that without ambiguity against the original,
loop-taking convention. None of these needed to change across any of the
five revisions above - they only ever consume `loop::current()`, never
`make_current()` itself, so only the registration side moved.

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
  work, all of it — with nothing able to preempt it. An earlier version
  timed each invocation itself, bracketing `node.run()` with two
  `platform::instance().now()` calls and printing straight from
  `run_one()` if the gap exceeded a threshold (currently 50ms, a starting
  point, not tuned against a real workload). That measuring/printing now
  lives on `platform::interface` instead —
  `reset_loop_stall_detection()`/`detect_loop_stall(threshold)` — for the
  same reason `now()`/`sleep_until()` are platform hooks rather than
  `est::loop` calling `std::chrono`/`std::this_thread` directly: "how do
  we know a callback ran long" is a policy a backend should get to answer
  for itself. `est::loop` only calls the two bracketing hooks and owns the
  threshold value; both are pure virtual on `platform::interface` itself
  (per repo owner review - see the `loop::current()` section above for the
  fuller story on why `interface` carries no default bodies or state of
  its own at all), so each concrete backend answers them itself.
  `hosted_stdcpp`'s own override just records `now()` on reset and
  compares against it on detect, printing via `platform::printdbg()` if
  exceeded — the same synchronous, single-threaded behavior an earlier
  version of this design had as `interface`'s shared default. A future
  backend could implement both to run a watchdog on a background thread
  instead, catching (and reporting) a stall in parallel while the callback
  is still running, rather than only finding out once it returns —
  without `run_one()` itself changing at all.

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
wrapping a generic closure (an earlier version, `concrete_timer_node<Fn>`,
did the latter) - needed so `destroy(allocator, ran)` can complete the
promise with an exception when `ran` is false (the timer never fired
before the loop was destroyed), instead of the generic version's silent
drop, which had exactly the stranded-coroutine-frame hazard described in
[Coroutines](Coroutines.md)'s `lock_resume_node`/`acquire_resume_node`
section - fixed here the same way (issue #50).

`yield_execution(loop&)` (issue #45) gives `loop_ref` the chance to run
whatever else is already ready before the calling coroutine resumes.
Originally sugar over `sleep_for(loop_ref, loop::clock::duration::zero())`
- landing in `pending_timers_` rather than `ready_` meant `run_impl()`'s
own loop (below) always fully drained `ready_` first, giving the right
ordering "for free," at the cost of a real timer round-trip
(`schedule_timer()`'s heap insert, `fire_ready_timers()`'s linear
search+erase, and the `platform::sleep_until()` call `run_impl()` makes
before it ever checks `pending_timers_`) for something with no actual
deadline to track.

Replaced once `est::intrusive_list<T>` became FIFO (below) with a direct
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
- the identical trick under this list's original LIFO policy would have
cut the new node in line ahead of everything else instead (see
[Architecture](Architecture.md) and `intrusive_list<T>`'s own doc
comment for that history). `yield_resume_node::destroy()` completes its
promise with an exception on abandonment rather than silently dropping
it, for the same reason `mutex::lock_resume_node`'s own doc comment
gives (`docs/wiki/Coroutines.md`) - a coroutine suspended via `co_await
yield_execution(loop);` holds the only other reference to its
`future_state<void>`, so silently dropping the promise would strand that
coroutine's frame forever if the loop is destroyed first.

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
