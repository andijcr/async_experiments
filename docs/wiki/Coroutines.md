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

## The calling convention: `est::loop&` first

Every `est::future<T>`-returning coroutine function must take `est::loop&`
as its first parameter. This isn't a style preference — it's what
`future<T>::promise_type`'s constructor and `operator new` pattern-match
against, via C++20's "promise constructor arguments" rule: the compiler
tries to construct `promise_type` from the same argument list the coroutine
was actually called with, before ever falling back to a default constructor.
`promise_type` here has no default constructor, so a coroutine that doesn't
supply `est::loop&` first fails to compile with "no matching constructor for
promise_type" rather than silently doing the wrong thing.

```cpp
class promise_type : public detail::future_promise_result<T> {
public:
  template <class... Args>
  explicit promise_type(loop& loop_ref, Args&... /*unused*/)
      : detail::future_promise_result<T>(
            shared_ptr<future_state<T>>::make(loop_ref.allocator(), loop_ref)),
        loop_(loop_ref) {}
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
(`read_and_double`'s `int input` above, say) — the pack is never read.

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

## Never running inline: `initial_suspend()`

Consistent with `future_state<T>::complete()` deferring every continuation
through `est::loop` instead of running it inline (M3), a coroutine's *first*
slice of work is deferred too. `initial_suspend()` always suspends and hands
resumption off to `est::loop`'s ready-queue:

```cpp
auto initial_suspend() noexcept -> detail::coroutine_start_awaiter {
  return detail::coroutine_start_awaiter(loop_);
}
```

Calling a coroutine function therefore never executes any of its body on the
caller's own stack — it returns a not-yet-ready `future<T>` immediately, and
the body only starts running once `loop.run_until_idle()`/`run()` drains the
ready-queue. This is exactly what makes coroutine-produced and
`then()`-chain-produced futures behave identically from a caller's
perspective.

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
codebase (`concrete_continuation<Fn, U>`, `concrete_timer_node<Fn>`). This
is also why `final_suspend()` safely uses plain `std::suspend_never` (the
coroutine frame self-destructs immediately on completion): nothing that
ever resumes the coroutine lives inside the frame being destroyed.

The [continuation node mechanism](Continuation-Node-Mechanism.md) page's own
"`bind_owner()` subtlety" section documents an earlier bug of the same
flavor (a design that looked right until traced through an object's actual
lifetime) — this is that pattern again, one layer up, in coroutine frames
instead of `future_state<T>` ownership.

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
  awaiter->>state: await_ready(): state.ready()?
  alt already ready
    awaiter->>coro: await_resume(): get() the value immediately
  else not ready
    awaiter->>node: await_suspend(handle): allocate
    awaiter->>state: set_continuation(node)
    Note over coro: suspended
    state->>state: (later) set_value()/set_exception() -> complete()
    state->>est_loop: enqueue_ready(node)
    est_loop->>est_loop: drain_ready(): ready_.dequeue()
    est_loop->>node: run() -> invoke() -> handle.resume()
    Note over coro: resumes here
    coro->>awaiter: await_resume(): get() the value (or rethrow)
  end
```

`future_awaiter<T>` needs its resumption node to satisfy
`future_state<T>::set_continuation()`'s signature — `detail::continuation_node<T>&`,
not the T-independent `ready_node` `coroutine_resume_node` derives — so it
gets its own tiny, otherwise-identical heap-allocated trampoline,
`future_resume_node<T>`, that ignores the `future_state<T>&` it's handed and
just resumes:

```cpp
template <class T> class future_resume_node final : public continuation_node<T> {
public:
  explicit future_resume_node(std::coroutine_handle<> handle) noexcept : handle_(handle) {}
  void invoke(future_state<T>&) override { handle_.resume(); }
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept override {
    allocator.delete_object(this);
  }
private:
  std::coroutine_handle<> handle_;
};
```

`await_resume()` moves the value out (`std::move(future_.get())`), matching
`future<T>::get()`'s own `&&`-qualified overload: `co_await` is inherently a
single-consumption use of whatever it's awaiting. On failure, `get()`
rethrows — which, thrown from inside `await_resume()`, the language treats
as equivalent to the exception being thrown from the `co_await` expression
itself, so it surfaces in the awaiting coroutine exactly like any other
exception crossing a `co_await`.

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

### Abandoned coroutines leak their frame

Both resumption nodes' `destroy()` are called two ways: after a successful
`run()`/`invoke()` (the normal case, described above), or by
`future_state<T>::~future_state()`/`loop::~loop()` draining whatever's left
when a `future_state`/`loop` is torn down without ever completing/draining
(the abandoned-future scenario `docs/PLAN.md`'s M2 section already
describes for `then()`). In that second case the coroutine handle was never
resumed, so its frame simply leaks — a coroutine, unlike a plain
`concrete_continuation<Fn, U>` node, has no other lifecycle event to free it
through. This mirrors the accepted cost of an abandoned `then()` chain, just
manifesting as a leaked frame instead of a harmless heap deallocation.

## `est::mutex::lock()` becomes awaitable

`est::mutex` was always bookkeeping-only (one `int` lock word, an intrusive
waiter list — see [Architecture](Architecture.md)), because nothing was
concurrent enough to need real protection. Coroutines introduce a genuine,
if still single-threaded, race: two coroutines each doing "read some shared
structure, `co_await` something, write it back" can interleave *at that
suspension point* and corrupt it — a cooperative-scheduling race, not the
interrupt-context reentrancy `est::mutex` was originally (and, per M1's own
revision, no longer is) built to guard against.

`co_await mutex.lock();` is built from exactly the same pieces as
`future<T>`'s own coroutine support — no new concepts, just applied to a
different queue:

```cpp
class mutex::lock_awaiter final {
public:
  [[nodiscard]] auto await_ready() noexcept -> bool {
    if (mutex_.locked()) return false;
    mutex_.state_ = 1;         // fast path: uncontended, acquire immediately
    return true;
  }
  void await_suspend(std::coroutine_handle<> handle) {
    auto* node = mutex_.loop_.allocator().new_object<lock_resume_node>(handle);
    mutex_.waiters_.enqueue(*node);   // slow path: queue a heap-allocated resume node
  }
  void await_resume() const noexcept {}
private:
  mutex& mutex_;
};
```

`unlock()` hands the lock directly to the next queued waiter (via
`loop.enqueue_ready()`, never an inline `handle.resume()` call — the same
"always deferred through the loop" reasoning as everywhere else, avoiding
unbounded call-stack growth for a chain of coroutines that each lock/unlock
in turn) rather than clearing the lock word first:

```cpp
void unlock() noexcept {
  if (auto* waiter = waiters_.dequeue()) {
    loop_.enqueue_ready(*waiter);   // ownership passes directly - no window where it reads free
    return;
  }
  state_ = 0;
}
```

Waiters resume in `est::intrusive_list`'s documented LIFO order — the most
recently queued coroutine is the first one handed the lock once it's free.

This is a breaking change from `est::mutex`'s earlier, synchronous
`lock()`/`unlock()` API: `lock()` can now only be meaningfully used via
`co_await`, from inside a coroutine — a bare `m.lock();` statement outside
one just constructs and discards an awaiter without acquiring anything.
`mutex` also now holds an `est::loop&` (the same M3 convention as
`future_state<T>`), needed to enqueue a waiter's resumption.
