# Coroutine support

M4 (`docs/PLAN.md`) adds C++20 coroutines to `est` — but deliberately not as
a new, separate type. This page covers the design choice that shapes
everything else here, the machinery behind it, a real bug it took to get the
machinery right, and how `est::mutex::lock()` became awaitable on top of the
same pieces.

## No `task<T>` — `est::future<T>` is the coroutine return type

The original M4 plan (`docs/PLAN.md`'s roadmap) called for an `est::task<T>`
coroutine type with its own `promise_type` binding to `est::promise<T>`/
`est::future<T>` "under the hood." That would have worked, but it means two
parallel vocabularies for "a value that becomes available later" — a caller
of a `then()`-chain-returning function gets a `future<T>`, a caller of a
coroutine gets a `task<T>`, and composing the two needs an explicit
conversion at the boundary.

Instead, `est::future<T>` itself is the coroutine return type, via a
`future<T>::promise_type` nested class. A function can be written as:

```cpp
est::future<int> read_and_double(est::loop& loop_ref, int input) {
  co_await est::sleep_for(loop_ref, 10ms);
  co_return input * 2;
}
```

and every caller — another coroutine doing `co_await read_and_double(...)`,
or ordinary code doing `.then()`/`.get()` after `loop.run_until_idle()` — is
looking at a plain `est::future<int>`, indistinguishable from one built out
of `sleep_for()`/`then()` chains. A caller genuinely cannot tell, from the
type alone or from calling-stack behavior, whether a `future<T>` came from a
coroutine or from ordinary continuation-passing code. That's the point.

## The calling convention: `est::loop&` first, or `est::current_loop()` (issue #30)

An `est::future<T>`-returning coroutine function can take `est::loop&` as
its first parameter - what `future<T>::promise_type`'s constructor and
`operator new` originally, and still, pattern-match against, via C++20's
"promise constructor arguments" rule: the compiler tries to construct
`promise_type` from the same argument list the coroutine was actually
called with, before ever falling back to a default constructor.

```cpp
class promise_type : public detail::future_promise_result<T> {
public:
  template <class... Args>
  explicit promise_type(loop& loop_ref, Args&... /*unused*/)
      : detail::future_promise_result<T>(
            shared_ptr<future_state<T>>::make(loop_ref.allocator(), loop_ref)) {}
  ...
  template <class... Args>
  static auto operator new(std::size_t size, loop& loop_ref, Args&... /*unused*/) -> void* {
    return detail::coroutine_frame_alloc(size, loop_ref.allocator());
  }
  static void operator delete(void* ptr, std::size_t size) noexcept {
    detail::coroutine_frame_dealloc(ptr, size);
  }
};
```

The trailing `Args&...` pack exists purely so this constructor/`operator
new` pair matches *whatever else* the actual coroutine function declares
(`read_and_double`'s `int input` above, say) — the pack is never read;
`loop_ref` itself isn't stored either, only used here to build `state_`.

**Issue #30** added two more ways to write a coroutine, for a caller
content relying on whichever loop is current (`est::current_loop()`,
[The Loop and Timers](Loop-And-Timers.md) - a free function, not a method
on `est::loop` itself, deliberately kept out of `loop.cppm` entirely)
instead of threading one through by hand - no `est::loop&` parameter at
all, or no parameters whatsoever:

```cpp
template <class First, class... Rest>
  requires(!std::same_as<std::remove_cvref_t<First>, loop>)
explicit promise_type(First& /*unused*/, Rest&... /*unused*/) : promise_type(current_loop()) {}

promise_type() : promise_type(current_loop()) {}
```

Both delegate to the `loop&`-taking constructor above rather than
repeating its initializer - per review, all three constructors should
share the one place that actually builds `state_`. That constructor is
already a template accepting an empty `Args...` pack, so
`promise_type(current_loop())` (a single `loop&` argument) already
matches it directly; the `requires`-constrained constructor is correctly
excluded from that call by its own clause, since the argument's type
genuinely is `loop`.

(plus the matching `operator new` pair, same shapes - not delegated the
same way, since each was already a single-line call straight to
`detail::coroutine_frame_alloc()`, with nothing left to deduplicate). The
`requires`
clause on the first is load-bearing, not decoration: without it, this
constructor and the `loop&`-taking one above would be genuinely
ambiguous for a call that *does* pass a loop first - a bare parameter
pack happily absorbs a leading `loop&` into `Rest` itself, so both
candidates would deduce to the identical actual parameter list
`(loop&, Rest&...)` for such a call, a tie conversion ranking alone
can't break. Excluding a leading `loop&` specifically (`std::same_as`,
matching the exact-type match the constructor above already relies on,
not a broader "convertible to") keeps the two mutually exclusive by
SFINAE instead of relying on any partial-ordering tie-break. A coroutine
taking no parameters at all needs its own non-template overload, since
the constrained one above still requires at least one `First`.

There's no default constructor with no `requires` clause at all - a
future<T>-returning coroutine always resolves to exactly one of the
three shapes above, based purely on its own parameter list, never
silently misbehaving.

`promise_type` doesn't hold its own `est::promise<T>` at all: it builds a
`shared_ptr<future_state<T>>` directly (the same call
`make_promise_future<T>(loop&)` makes internally) and talks to it straight
through `return_value()`/`return_void()`/`unhandled_exception()`. A
coroutine's `promise_type` *is* the producer side — `est::promise<T>` stays
exactly as it was, for the non-coroutine producer path (`sleep_for()`, or
any hand-written code that calls `set_value()` directly).

### `return_value`/`return_void`: one base per T-is-void fork

A `promise_type` can't define both `return_value()` and `return_void()` —
the compiler treats providing both as an error, and which one a coroutine's
`co_return` needs depends on whether `T` is `void`. Rather than duplicating
`future<T>::promise_type` as two near-identical `T`/`void` specializations,
that one fork is split into a small base class:

```cpp
template <class T> class future_promise_result {
public:
  void return_value(const T& value) { state_->set_value(value); }
  void return_value(T&& value) { state_->set_value(std::move(value)); }
protected:
  shared_ptr<future_state<T>> state_;
};

template <> class future_promise_result<void> {
public:
  void return_void() { state_->set_value(); }
protected:
  shared_ptr<future_state<void>> state_;
};
```

`future<T>::promise_type : public detail::future_promise_result<T>` picks up
exactly the right one — the same "resolve the void fork once, in a small
dedicated place" approach `future_state<T>::stored_t` already uses (see
[Continuation Node Mechanism](Continuation-Node-Mechanism.md)).

## `initial_suspend()`: running synchronously up to the first real suspension

```cpp
auto initial_suspend() noexcept -> std::suspend_never { return {}; }
```

Calling a coroutine function runs its body immediately, on the caller's own
stack, up to its first genuine suspension point (a real `co_await` on
something not yet ready) or all the way to `co_return`/an uncaught
exception if it never awaits anything at all — exactly like an ordinary
function computing a value before handing back a `future<T>`. A
`[](loop&) -> future<int> { co_return 42; }` coroutine, for instance, never
touches `est::loop` at all: the call returns an already-`ready()` future
directly, no `run_until_idle()` needed to observe it.

This wasn't the original design. An earlier version had `initial_suspend()`
always suspend, deferring even a coroutine's very first slice of work
through `est::loop`'s ready-queue - consistent, at the time, with
`future_state<T>::complete()` deferring every continuation instead of
running it inline (M3), and with the matching stance `operator co_await()`
(below) took on the *awaiting* side. A PR review discussion (PR #37)
revisited that: the repo owner's framing was that a coroutine's synchronous
prefix, up to its first real suspension point, is conceptually the same as
the work an ordinary function does before returning a future - and paying
for a heap-allocated resume node plus a full ready-queue round trip to kick
off that prefix, when there's nothing to actually interleave with, wasn't
worth what it bought. The concrete cost was traced precisely before making
the change: exactly one allocation and one round trip per coroutine call,
regardless of how much real work or how many suspension points the
coroutine has - every actual `co_await` still allocates its own resume
node independently, unaffected by what `initial_suspend()` does. Once
`operator co_await()` (below) adopted the same "don't wait for something
that isn't being waited for" stance for the identical reason, keeping
`initial_suspend()` on the older, always-deferred policy would have been
the inconsistent choice, not the safe one.

One real consequence: a caller of a coroutine-returning function can no
longer assume the `future<T>` it gets back is never already resolved, with
side effects already applied, before it was ever inspected - a plain
function call already carries that same property (nothing stops an
ordinary function from doing arbitrary work before returning), so this
isn't a new kind of risk, just coroutines no longer being exempt from it.

## The bug: an awaiter can't safely resume-then-be-destroyed from inside its own frame

The first version of this design tried to avoid a heap allocation for the
"resume this coroutine" step: `coroutine_start_awaiter` (returned by
`initial_suspend()`) derived `est::detail::ready_node` directly and enqueued
*itself* onto the loop, reasoning that a co_await's awaiter object persists
across suspension (the language guarantees this) so there was nothing extra
to allocate.

That reasoning is correct for the *duration of the awaiter's own co_await
expression* — and nowhere else. `est::loop::run_one()` calls `node.run()`
and then, via a `scope_exit` guard, `node.destroy(allocator_)` — *after*
`run()` has already returned:

```cpp
void run_one(detail::ready_node& node) {
  const auto guard = destroy_guard(node);
  ...
  node.run();
  ...
} // guard fires here, calling node.destroy(allocator_)
```

If `node` is a `ready_node` embedded in the very coroutine frame that
`run()`'s `handle_.resume()` call just resumed, `run()` resuming the
coroutine *past its own suspension point* means the compiler is now free to
reuse that exact frame storage for whatever the coroutine's later code
constructs — its next awaiter, a local variable — since the two objects'
lifetimes don't overlap. By the time `destroy_guard`'s destructor calls
`node.destroy(...)`, that memory may already hold something else entirely.
Calling a virtual function through it is undefined behavior.

This wasn't theoretical — reproduced directly, with `libc++abi` reporting
`Pure virtual function called!`: a virtual dispatch through a vtable pointer
that had already been clobbered by the coroutine's own subsequent frame
activity.

The fix, applied uniformly everywhere this pattern appears
(`coroutine_start_awaiter`, `future<T>`'s `operator co_await()` awaiter, and
`mutex::lock()`'s awaiter — see below): the awaiter itself stays a
coroutine-frame subobject (fine — nothing reaches back into it after its own
co_await expression ends), but `await_suspend()` allocates a small,
*separately heap-allocated* resumption node and registers that instead of
registering itself:

```cpp
class coroutine_resume_node final : public ready_node {
public:
  explicit coroutine_resume_node(std::coroutine_handle<> handle) noexcept : handle_(handle) {}
  void run() final { handle_.resume(); }
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept final {
    allocator.delete_object(this);
  }
private:
  std::coroutine_handle<> handle_;
};

class coroutine_start_awaiter {
public:
  explicit coroutine_start_awaiter(loop& loop_ref) noexcept : loop_(loop_ref) {}
  [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }
  void await_suspend(std::coroutine_handle<> handle) {
    auto* node = loop_.allocator().new_object<coroutine_resume_node>(handle);
    loop_.enqueue_ready(*node);
  }
  void await_resume() const noexcept {}
private:
  loop& loop_;
};
```

A separately allocated node has its own independent lifetime, entirely
unrelated to the coroutine frame it resumes — `run()`-then-`destroy()` is
exactly as safe here as it already is for every other `ready_node` in this
codebase (`concrete_continuation<Fn, U>`, `est:promise`'s `sleep_resume_node`/
`yield_resume_node`). This
is also why `final_suspend()` safely uses plain `std::suspend_never` (the
coroutine frame self-destructs immediately on completion): nothing that
ever resumes the coroutine lives inside the frame being destroyed.

The [continuation node mechanism](Continuation-Node-Mechanism.md) page's own
"`bind_owner()` subtlety" section documents an earlier bug of the same
flavor (a design that looked right until traced through an object's actual
lifetime) — this is that pattern again, one layer up, in coroutine frames
instead of `future_state<T>` ownership.

`coroutine_start_awaiter`/`coroutine_resume_node` themselves no longer
exist in the current code - once `initial_suspend()` stopped needing an
awaiter at all (it just returns `std::suspend_never` directly now, per the
section above), there was nothing left for them to do. The lesson they
were built to demonstrate still applies directly, unchanged, to
`future_awaiter<T>`/`future_resume_node<T>` (below) and to
`mutex::lock_resume_node`/`acquire_resume_node` (further down this page) -
every one of them keeps the identical "awaiter stays a frame subobject,
resumption node is separately heap-allocated" split, for exactly this
reason.

## `operator co_await` — awaiting *any* `future<T>`

The other half of the design: a coroutine can `co_await` any
`est::future<T>`, regardless of what produced it.

```cpp
[[nodiscard]] auto operator co_await() noexcept -> detail::future_awaiter<T>;
```

```mermaid
sequenceDiagram
  participant coro as awaiting coroutine
  participant awaiter as future_awaiter&lt;T&gt;
  participant state as awaited future_state&lt;T&gt;
  participant node as future_resume_node&lt;T&gt;
  participant est_loop as est::loop

  coro->>awaiter: co_await someFuture
  awaiter->>state: await_ready(): future_.ready()
  alt already ready
    Note over coro: resumes inline, no suspension at all
  else not ready yet
    awaiter->>node: await_suspend(handle): allocate
    awaiter->>state: set_continuation(node)
    Note over coro: suspended
    state->>state: waiters_.enqueue(node)
    Note over state,node: (later) set_value()/set_exception() -> complete()
    state->>est_loop: enqueue_ready(node)
    est_loop->>est_loop: drain_ready(): ready_.dequeue()
    est_loop->>node: run() -> invoke() -> handle.resume()
    Note over coro: resumes here
  end
  coro->>awaiter: await_resume(): get() the value (or rethrow)
```

`await_ready()` returns `future_.ready()` directly - `co_await` on an
already-ready future skips suspension entirely, resuming the rest of the
awaiting coroutine's body immediately, right there on whatever call stack
reached that `co_await`. This wasn't the original design: an earlier
version unconditionally returned `false`, deliberately matching a
tested invariant this codebase held `then()` to at the time - even an
already-ready `then()` registration defers through `est::loop` rather
than running inline (`future_tests.cpp`, *"then() registered on an
already-ready future still defers to the loop"*, still true and
unchanged). That was reversed in the same PR #37 review round that
changed `initial_suspend()` (above), for the identical reasoning: paying
for a `future_resume_node<T>` allocation and a full ready-queue round
trip purely to resume something that was never actually going to wait for
anything stopped being worth it, once `initial_suspend()` itself had
already made the same call at the other end of a coroutine's lifetime.
`future_state<T>::set_continuation()` (called from `await_suspend()`)
still handles the "not yet ready" case exactly as before - this only
changes whether that call, and the node it needs, happens at all, which
is what the diagram's `alt` now shows.

`future_awaiter<T>` needs its resumption node to satisfy
`future_state<T>::set_continuation()`'s signature —
`detail::continuation_node<T>&` — so it gets its own tiny heap-allocated
trampoline, `future_resume_node<T>`, that ignores the
`const shared_ptr<future_state<T>>&` it's handed and just resumes:

```cpp
template <class T> class future_resume_node final : public continuation_node<T> {
public:
  explicit future_resume_node(std::coroutine_handle<> handle) noexcept : handle_(handle) {}
  void invoke(const shared_ptr<future_state<T>>&) override {
    handle_.resume();
  }
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept override {
    if (!ran) {
      handle_.destroy();   // see "Abandoned coroutines are destroyed, not leaked" below
    }
    allocator.delete_object(this);
  }
private:
  std::coroutine_handle<> handle_;
  bool invoked_ = false;
};
```

`await_resume()` moves the value out (`std::move(future_.get())`), matching
`future<T>::get()`'s own `&&`-qualified overload: `co_await` is inherently a
single-consumption use of whatever it's awaiting. On failure, `get()`
rethrows — which, thrown from inside `await_resume()`, the language treats
as equivalent to the exception being thrown from the `co_await` expression
itself, so it surfaces in the awaiting coroutine exactly like any other
exception crossing a `co_await`.

### Why `future<T>` can't be copyable

`future<T>` is a thin wrapper around a `shared_ptr<future_state<T>>`, and
`shared_ptr` itself is freely copyable — so it's a fair question why
`future<T>` isn't too; a copy would just be another handle to the same
`future_state`, which sounds like exactly what `co_await`ing the same
result from two places would want. The reason lives in what `get()` does
with the value once it's there, not in the shared_ptr underneath it.

`future<T>::get()` is deducing-this, and the two branches don't do the
same thing:

```cpp
template <class Self> [[nodiscard]] auto get(this Self&& self) -> T {
  if constexpr (std::is_lvalue_reference_v<Self>) {
    return self.state_->get();            // future_state::get() lvalue path -> T&, copied into the by-value return
  } else {
    return std::move(*self.state_).get(); // future_state::get() rvalue path -> T&&, moved into the by-value return
  }
}
```

The lvalue path (`future.get()`) *copies* the stored value out of
`future_state<T>::result_` — harmless and repeatable, which is exactly why
`.then()`'s unwrapped dispatch (`state.get()` on an lvalue
`future_state<T>&`, [Continuation Node
Mechanism](Continuation-Node-Mechanism.md#the-two-calling-conventions))
can register any number of independent continuations against one
`future_state` and let every one of them read the result safely, however
many there are. The rvalue path (`std::move(future).get()`) *moves* it
out instead, and a move doesn't reset `result_` — it just leaves whatever
moved-from state `T` ends up in sitting there permanently, since nothing
else ever touches that variant again. `await_resume()` above always takes
this path.

So: if `future<T>` were copyable, nothing would stop two copies each
`co_await`ing their own handle to the same `future_state`. The first one
to run gets the real value; the second gets `T`'s moved-from state —
silently, no exception, no diagnostic, just wrong data if `T` isn't
trivially copyable enough for "moved-from" to coincidentally look like
"unchanged" (an `int` wouldn't even show it; a `std::string` or a
`std::vector` would). That's a materially worse failure mode than a
compile error, so `future<T>`'s copy constructor stays deleted and its
move constructor is `= default`, matching `std::future<T>`'s own
single-consumer contract rather than `std::shared_future<T>`'s multi-
consumer one (which sidesteps this exact problem by having `get()` return
a `const T&`/copy, never a move, in exchange for requiring `T` be
copyable at all).

Two details worth being explicit about, since they rule out the more
clever-looking fixes:

- **A decision made once, when the future_state becomes ready, isn't
  enough.** `future_state<T>::set_continuation()` explicitly supports a
  waiter registering *after* the future is already resolved — it's the
  fast path every already-satisfied `co_await`/`then()` takes:
  ```cpp
  void set_continuation(continuation_node& node, const shared_ptr<future_state>& self) {
    if (ready()) {
      node.bind_owner(self);
      loop_.enqueue_ready(node);
      return;
    }
    waiters_.enqueue(node);
  }
  ```
  So "how many waiters exist when `set_value()`/`complete()` runs" isn't
  "how many will ever exist" — a value moved out (or discarded) right
  then based on that count would break any consumer that shows up later.
  `result_` has to stay in `future_state` for as long as the
  `future_state` itself lives, which is exactly what the always-copying
  `.then()` path relies on.
- **A refcount check at each `get()` call doesn't line up with when
  consumption actually happens either.** `future_resume_node<T>` (above)
  binds its own `shared_ptr<future_state<T>>` reference before being
  queued, and [`est::loop::run_one()`](Loop-And-Timers.md) doesn't destroy
  that node until *after* `run()` returns - which is also after
  `await_resume()` (called from inside `run()`'s own `handle_.resume()`)
  has already run. A "move only if I'm the sole reference" check made
  from inside `await_resume()` would almost never see a count of one,
  even for the genuinely-only-one-waiter case:
  the resume node driving that exact resumption is still holding its own
  reference at that point.

The general shape of "move if you're the last owner, copy otherwise" is a
real, useful pattern elsewhere (`Rc::try_unwrap` in Rust; copy-on-write
strings) — it just doesn't have a sound place to hook into *this*
specific design without changing what triggers a consumption in the first
place. `.then()`, already always on the non-consuming path regardless of
count, is the existing answer for "more than one independent reaction to
one result"; `future<T>::clone()`
([issue #26](https://github.com/andijcr/async_experiments/issues/26))
makes that available to an arbitrary caller, not just `then()`'s own
internals:

```cpp
[[nodiscard]] auto clone() const -> future { return future(state_); }
```

A clone is another `future<T>` aliasing the same `future_state` — both
see the same eventual result, and either may safely call the lvalue
(copying) `get()` or register any number of `then()` callbacks, any
number of times. It does *not* make the rvalue (consuming) `get()` path
safe across clones of a value-carrying `future<T>`: `co_await` always
takes that path (`await_resume()`, above), so a second clone's `co_await`
still reads the first's moved-from leftovers, for exactly the reasons
this section just walked through — `clone()` hands out another handle to
the same `future_state`, it doesn't change what consuming that state's
value means. It's unconditionally safe when `T` is `void` (nothing to
consume, so every clone's `co_await` is just a "when is this ready"
signal) or when every clone sticks to the non-consuming paths (`get()` on
an lvalue, `then()`). This is exactly the shape `est::event` (issue #55)
needs: one `future<void>`, cloned once per waiter, each independently
`co_await`-able.

Implementing `clone()` also removed the one place this codebase used to
call `shared_from_this()` from inside `future_state<T>` itself
(`set_continuation()`'s ready-now fast path, and the wrapped-mode `then()`
view shown in the [Continuation Node
Mechanism](Continuation-Node-Mechanism.md) page) — every one of those call
sites is reached from code that already holds a `shared_ptr<future_state<T>>`
pointing at the same object (a `promise<T>`/`future<T>`'s own `state_`, or a
`continuation_node`'s own `owner_`), so `future_state<T>` no longer inherits
`est::enable_shared_from_this` at all; `set_value()`/`set_exception()`/
`set_continuation()`/`then()` each take that `shared_ptr` as an explicit
`self` parameter instead.

### Composability, concretely

Because `co_await` works uniformly on any `future<T>`, a coroutine can await
another coroutine's result directly:

```cpp
est::future<int> inner(est::loop& loop_ref, int x) { co_return x * 2; }

est::future<int> outer(est::loop& loop_ref) {
  const int value = co_await inner(loop_ref, 21);
  co_return value + 1;
}
```

or a `then()`-chain future built by ordinary, non-coroutine code:

```cpp
auto [promise, future] = est::make_promise_future<int>(loop);
auto chained = future.then([](int v) { return v + 1; });

est::future<int> coro(est::loop&, est::future<int> fut) {
  const int value = co_await std::move(fut);
  co_return value * 10;
}
coro(loop, std::move(chained));
```

Neither `inner`'s caller nor `chained`'s consumer needs to know or care that
the other side is a coroutine.

### Abandoned coroutines are destroyed, not leaked

Every resumption node's `destroy()` is called two ways: after a successful
`run()`/`invoke()` (the normal case, described above), or by
`future_state<T>::~future_state()`/`loop::~loop()` draining whatever's left
when a `future_state`/`loop` is torn down without ever completing/draining
(the abandoned-future scenario `docs/PLAN.md`'s M2 section already
describes for `then()`). A first version of this code got the second case
wrong — `destroy()` freed only the resumption node itself, never the
coroutine frame the handle pointed to, permanently leaking it (caught in
code review, before merging, not after). The fix: each resumption node's
`destroy()` is told whether it ever actually ran -

```cpp
template <class T> class future_resume_node final : public continuation_node<T> {
public:
  void invoke(const shared_ptr<future_state<T>>&) override {
    handle_.resume();
  }
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept override {
    if (!ran) {
      handle_.destroy();   // never resumed - still fully intact; this is
                            // the only chance to free its frame
    }
    allocator.delete_object(this);
  }
  ...
};
```

If `run()`/`invoke()` never happened, the coroutine is still exactly where
`await_suspend()` left it - fully intact, suspended, never touched -
so destroying it here is both safe and necessary. If `run()`/`invoke()`
*did* happen, this `destroy()` call must not touch `handle_` again: the
coroutine either already self-destroyed (`promise_type::final_suspend()`'s
`std::suspend_never` - `handle_` is now dangling, so even calling `.done()`
on it would be a use-after-free) or suspended again on something else
entirely, which now owns resuming (and eventually destroying) it. `ran` is
what lets one `destroy()` implementation tell those two completely
different situations apart without ever having to safely query a handle
that might already be gone - passed in by the caller (`ready_node::
destroy()`'s own doc comment, est:loop) rather than tracked with a private
flag each node sets on its own `run()`/`invoke()`, since every call site
already knows statically which situation it's in.

## `est::mutex::lock()` becomes awaitable

`est::mutex` was always bookkeeping-only (one `int` lock word, an intrusive
waiter list — see [Architecture](Architecture.md)), because nothing was
concurrent enough to need real protection. Coroutines introduce a genuine,
if still single-threaded, race: two coroutines each doing "read some shared
structure, `co_await` something, write it back" can interleave *at that
suspension point* and corrupt it — a cooperative-scheduling race, not the
interrupt-context reentrancy `est::mutex` was originally (and, per M1's own
revision, no longer is) built to guard against.

An early version of `lock()` returned a bespoke awaiter type
(`lock_awaiter`) with its own hand-rolled `await_ready()`/`await_suspend()`/
`await_resume()`, so an uncontended lock could be acquired with zero
allocations and no genuine suspension at all. A PR review discussion
(PR #37) concluded that fast path was actually an *inconsistency*, not a
optimization worth keeping: every other producer in this codebase -
`then()`, `future_awaiter<T>` (after its own review-round fix, below), and
`promise_type::initial_suspend()` - pays a fixed allocation cost
specifically so a caller never has to wonder whether a given `future<T>`
might already be resolved, with side effects already applied, before it
was ever inspected. `lock_awaiter`'s fast path was the one place left that
broke that guarantee. `lock()` now just returns a plain `future<void>`,
built the same way `acquire()` (below) is:

```cpp
[[nodiscard]] auto lock() -> future<void> {
  auto [prom, fut] = make_promise_future<void>(loop_);
  if (state_ == 0) {
    state_ = 1;
    prom.set_value();               // fast path: uncontended, acquire immediately
    return std::move(fut);
  }
  auto* node = loop_.allocator().template new_object<lock_resume_node>(std::move(prom));
  waiters_.enqueue(*node);          // slow path: queue a heap-allocated resume node
  return std::move(fut);
}
```

`co_await mutex.lock();` still works exactly as before, syntactically -
`future<T>` is awaitable from any coroutine (`operator co_await()`,
above). What actually happens underneath shifted twice across this same
PR review, in opposite directions:

1. At the point `lock()` was first converted to `future<void>`,
   `future_awaiter<T>::await_ready()` still unconditionally returned
   `false` (its own review-round fix, above, hadn't been revisited yet) -
   so *every* `co_await mutex.lock()` genuinely suspended and deferred
   through the loop, uncontended or not. Concretely: an uncontended
   `lock()` went from 0 allocations/0 loop round-trips (`lock_awaiter`'s
   old fast path) to 2 allocations (the `future_state<void>` control
   block, plus the `future_resume_node<T>` `await_suspend()` must
   allocate regardless of readiness) and 1 round-trip; a contended
   `lock()` went from 1 allocation/1 round-trip (`lock_resume_node`
   resuming the handle directly) to 3 allocations and 2 round-trips.
2. Once `future_awaiter<T>::await_ready()` itself was changed to check
   `future_.ready()` directly (same section above), `lock()`'s fast path
   got most of that back *for free*, with no `lock_awaiter` needed at
   all: an uncontended `co_await mutex.lock()` resumes inline, no
   `future_resume_node<T>` allocated and no loop round-trip, since
   `await_suspend()` never runs when `await_ready()` already returns
   `true`. The one allocation `lock_awaiter`'s original fast path avoided
   that this version still pays is the `future_state<void>` control block
   itself, from `make_promise_future<void>()` - `lock()` builds one
   unconditionally, fast path or not, since it has no way to hand back a
   `future<void>` without one.

`lock()` is also no longer awaitable-*only*: since it returns a plain
`future<void>`, it can be used from ordinary, non-coroutine code too
(polled via `ready()`/`get()`, or chained with `then()`), not just via
`co_await`.

`unlock()` hands the lock directly to the next queued waiter (via
`loop.enqueue_ready()`, never completing its promise inline here — the
same "always deferred through the loop" reasoning as everywhere else,
avoiding unbounded call-stack growth for a chain of coroutines that each
lock/unlock in turn) rather than clearing the lock word first:

```cpp
void unlock() noexcept {
  if (auto* waiter = waiters_.dequeue()) {
    loop_.enqueue_ready(*waiter);   // ownership passes directly - no window where it reads free
    return;
  }
  state_ = 0;
}
```

Waiters resume in `est::intrusive_list`'s documented FIFO order — the
first-queued waiter is the first one handed the lock once it's free.

### `lock_resume_node`/`acquire_resume_node`: completing on abandonment, not just dropping

`~mutex()` drains `waiters_` the same way `future_state<T>`'s and
`est::loop`'s own destructors drain theirs — a mutex destroyed with a
waiter still queued on `lock()`/`acquire()` must not just leak it. With
the earlier, coroutine-handle-holding `lock_resume_node`, that was enough
on its own: the node held the coroutine handle directly, so destroying
the node (with a `ran_` flag to avoid double-destroying an
already-resumed one) was sufficient to free the frame.

Once `lock_resume_node` holds a `promise<void>` instead of a
`coroutine_handle<>`, that direct line is gone, and naively deallocating
an abandoned node (matching the "broken promise, the future simply never
becomes ready" contract every other dropped `est::promise<T>` in this
codebase otherwise has by default) reopens a real leak: a coroutine doing `co_await
mutex.lock();` holds the resulting `future<void>` as a temporary spilled
into its own frame across the suspension - the *only* other reference to
that `future_state<void>`, besides the node in `mutex::waiters_`. Drop
the node's promise silently, and the future_state is left forever
"not yet ready," kept alive solely by the very coroutine frame that can
only ever be freed by that future_state eventually completing. Neither
side can free the other first - not a `shared_ptr` cycle in the strict
sense (no object holds a `shared_ptr` back to the thing keeping it
alive), but a practical one: both stay allocated forever.

`destroy()` breaks that by actually completing the promise (with an
exception, since the true outcome is "this waiter never got the lock, and
the mutex it was queued on no longer exists") before deallocating:

```cpp
void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept final {
  if (!ran) {
    promise_.set_exception(std::make_exception_ptr(
        std::runtime_error("mutex destroyed while lock() was pending")));
  }
  allocator.delete_object(this);
}
```

Completing the promise drains the future_state's own pending continuation
(the coroutine's `future_resume_node<T>`) onto `est::loop`'s `ready_`
queue - where it's either genuinely resumed (if the loop outlives this
mutex and keeps running - the coroutine then observes the exception
exactly like any other failure propagating across `co_await`, per
"an exception in the awaited future propagates across `co_await`" above)
or safely destroyed, never run, by `est::loop`'s own destructor-time
drain - either way, the frame is no longer stranded. `ran` (the same
parameter `est:future`'s own `future_resume_node<T>` reads, passed in by
the caller rather than tracked with a private flag - `ready_node::
destroy()`'s own doc comment, est:loop) is what stops this from
double-completing an already-successfully-completed promise: `run()`
and `destroy()` are
always both called, in that order, for any node the loop actually
processes (see [Continuation Node Mechanism](Continuation-Node-Mechanism.md)).

### `acquire()`: the same lock, packaged as a RAII `future<lock_guard>`

`lock()`/`unlock()` stay as shown above — a deliberately low-level, manual
pair (repo owner's own call on a PR review). `acquire()` sits alongside
them for a caller who'd rather not have to remember the matching
`unlock()` call:

```cpp
[[nodiscard]] auto acquire() -> future<lock_guard> {
  auto [prom, fut] = make_promise_future<lock_guard>(loop_);
  if (state_ == 0) {
    state_ = 1;
    prom.set_value(lock_guard(*this));
    return std::move(fut);
  }
  auto* node = loop_.allocator().template new_object<acquire_resume_node>(*this, std::move(prom));
  waiters_.enqueue(*node);
  return std::move(fut);
}
```

`lock_guard` is a move-only handle (`future<T>`/`promise<T>`'s own
convention) whose destructor calls `unlock()` — suppressed after a move,
the same "empty after move" contract `shared_ptr<T>` already documents.
`acquire()` is deliberately *not* a coroutine itself: every `future<T>`-
returning coroutine in this codebase takes `est::loop&` as an explicit
first parameter (the calling-convention section above), which would make
`co_await mutex.acquire()` an oddly-shaped call for something `mutex`
already has its own `loop&` for internally. Instead it's built the same
way `lock()` is - no coroutine frame, no `promise_type`, just a plain
function that either completes the promise immediately (the fast path) or
queues a waiter node that completes it later.

`acquire_resume_node` sits in the exact same `waiters_` list
`lock_resume_node` does (`intrusive_list<detail::ready_node>` doesn't care
which concrete type it holds) - `unlock()` hands the lock to whichever
one is next in FIFO order without needing to know which kind it got. It
needs the same `ran`-guarded exception-completion `destroy()` that
`lock_resume_node` does, for the identical reason - a coroutine doing
`auto guard = co_await mutex.acquire();` holds the same kind of
frame-spilled `future<lock_guard>` temporary across its own suspension,
so an abandoned `acquire_resume_node` has to complete its promise, not
just drop it, or that coroutine's frame leaks exactly the same way. The
only real difference from `lock_resume_node` is that its promise carries
a `lock_guard` value on success, which needs a `mutex&` to construct.

Because `future<T>` is awaitable from *any* coroutine, `acquire()`'s
result can be `co_await`ed from inside one - `auto guard = co_await
mutex.acquire();` - exactly like `lock()`, it just isn't required to be.

### `unlock()`: deferred through the loop, not completed inline - and a known, open limitation

`unlock()` hands a dequeued waiter to `est::loop::enqueue_ready()` rather
than calling `run()`/`destroy()` on it directly:

```cpp
void unlock() noexcept {
  if (auto* waiter = waiters_.dequeue()) {
    loop_.enqueue_ready(*waiter);
    return;
  }
  state_ = 0;
}
```

A later PR #37 review pass flagged a real hazard in this: `acquire_resume_
node::run()` needs a live `mutex&` to construct the `lock_guard` it
completes with, and deferring that construction to a later loop drain
leaves a window - between `unlock()` enqueuing the node and the loop
actually draining it - during which the mutex could be destroyed (nothing
about "the loop outlives the mutex" precondition rules this out; a mutex
is free to have a shorter lifetime than its loop). A queued waiter drained
by `run()` after that point would construct a `lock_guard` over an
already-dangling `mutex&`.

The obvious fix - have `unlock()` call `run()` (then `destroy()`) on the
waiter directly, reasoning that `set_value()`'s own `complete()`
(`est:future`) never runs anything synchronously, so completing here
still wouldn't run the waiter's own downstream code on this stack - was
implemented and then reverted after the `sanitize` preset (ASan+UBSan)
caught a *worse* regression it introduced. With that change,
`acquire_resume_node::run()` always constructed a real `lock_guard`
synchronously inside `unlock()`, even for a waiter that turned out to be
a coroutine later abandoned (destroyed without ever consuming its
`future<lock_guard>`, e.g. via `~loop()`'s own destructor-time drain of a
still-suspended frame). Abandonment is only safe because `destroy()`
(never `run()`) completes with an exception instead of a real value - see
the previous section - and that guard rail stopped applying once `run()`
ran unconditionally inside `unlock()`, ahead of whether the waiter would
ever actually be consumed: the resulting `lock_guard`, now stored inside a
`future_state<lock_guard>` with no consumer, would eventually have its
destructor run by the abandoned frame's own teardown and call `unlock()`
on a `mutex` that, in the failing test, had already been destroyed. ASan
reported this as a stack-use-after-scope in
"destroying a mutex with a coroutine co_await-ing acquire() still pending
leaks nothing" - a scenario this codebase already deliberately supports
for every other `future<T>`, and one considerably easier to hit in
practice than the original "loop outlives a shorter-lived mutex" hazard
the fix targeted. Deferring through the loop keeps abandonment's guard
rail intact: whether a queued waiter is later `run()` (drained normally)
or `destroy()`'d (the loop or the mutex is torn down first) is decided at
drain time by whichever actually happens first, not decided in advance
inside `unlock()`.

Neither design is airtight - each protects one abandonment/lifetime
scenario at the cost of the other - so this was kept as a documented,
open limitation rather than "solved" with the fix that traded one
confirmed use-after-free for a different one:

> **Known limitation.** `acquire_resume_node::run()` needs a live
> `mutex&`. If a mutex is destroyed after `unlock()` enqueues a waiter but
> before the loop actually drains it - only reachable if the loop
> outlives that mutex and keeps running during the gap, since `~mutex()`
> itself drains `waiters_` via `destroy()`, never `run()` - that waiter's
> eventual `run()` constructs a `lock_guard` over an already-dangling
> `mutex&`. Precondition instead of a fix: the loop must outlive every
> mutex constructed against it, and a caller must not let the loop keep
> running past a mutex's destruction while a waiter is still queued on it.
