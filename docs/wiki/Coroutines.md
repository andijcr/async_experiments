# Coroutine support

`est` adds C++20 coroutine support — but deliberately not as
a new, separate type. This page covers the design choice that shapes
everything else here, the machinery behind it, why a resume node must be
separately heap-allocated rather than embedded in the coroutine frame it
resumes, and how `est::mutex::lock()` is awaitable on top of the
same pieces.

## No `task<T>` — `est::future<T>` is the coroutine return type

An `est::task<T>` coroutine type with its own `promise_type` binding to
`est::promise<T>`/`est::future<T>` "under the hood" would have worked, but
it would mean two parallel vocabularies for "a value that becomes
available later" — a caller of a `then()`-chain-returning function gets a
`future<T>`, a caller of a coroutine gets a `task<T>`, and composing the
two needs an explicit conversion at the boundary.

Instead, `est::future<T>` itself is the coroutine return type, via a
`future<T>::promise_type` nested class. A function can be written as:

```cpp
est::future<int> read_and_double(int input) {
  co_await est::sleep_for(10ms);
  co_return input * 2;
}
```

and every caller — another coroutine doing `co_await read_and_double(...)`,
or ordinary code doing `.then()`/`.get()` after `loop.run_until_idle()` — is
looking at a plain `est::future<int>`, indistinguishable from one built out
of `sleep_for()`/`then()` chains. A caller genuinely cannot tell, from the
type alone or from calling-stack behavior, whether a `future<T>` came from a
coroutine or from ordinary continuation-passing code. That's the point.

## The calling convention: always `est::current_loop()`

An `est::future<T>`-returning coroutine function never takes an explicit
`loop&` that `promise_type` actually reads - `current_loop()`
([The Loop and Timers](Loop-And-Timers.md) - a free function, not a
method on `est::loop` itself, deliberately kept out of `loop.cppm`
entirely) is the only loop `promise_type` ever builds against, whatever
parameter list the coroutine function itself declares:

```cpp
class promise_type : public detail::future_promise_result<T> {
public:
  template <class... Args>
  explicit promise_type(Args&... /*unused*/) : detail::future_promise_result<T>(make_state()) {}
  ...
  template <class... Args>
  static auto operator new(std::size_t size, Args&... /*unused*/) -> void* {
    return detail::coroutine_frame_alloc(size, current_allocator());
  }
  static void operator delete(void* ptr, std::size_t size) noexcept {
    detail::coroutine_frame_dealloc(ptr, size);
  }

private:
  static auto make_state() -> shared_ptr<future_state<T>> {
    return shared_ptr<future_state<T>>::make(current_allocator());
  }
};
```

`current_allocator()` (est:util.current_loop), not
`current_loop().allocator()`: a direct `thread_local` read of the cached
`memory_resource*` instead of one `thread_local` read for the loop
pointer plus a further memory read through it for its allocator - see
[Loop and Timers](Loop-And-Timers.md) for the full mechanism.

The trailing `Args&...` pack exists purely so this one constructor/
`operator new` pair matches *whatever* the actual coroutine function
declares (`read_and_double`'s `int input` above, say, or nothing at all)
via C++20's "promise constructor arguments" rule (the compiler tries to
construct `promise_type` from the same argument list the coroutine was
actually called with, before ever falling back to a default
constructor) - the arguments themselves are never read.

This used to be three constructor/`operator new` pairs, resolved by
overload/SFINAE dispatch on whether the coroutine's first parameter was
exactly `est::loop&`: one pattern-matched a leading `loop&` and built
`future_state<T>` against it directly, the other two fell back to
`current_loop()`. Collapsing to the single shape above is itself the
result of an experiment (branch `current-loop-only-experiment`) that
removed every explicit-`loop&`-taking constructor/factory function across
this codebase, in favor of resolving `current_loop()` everywhere - see
[Loop and Timers](Loop-And-Timers.md) for the correctness hazard that
surfaced from it and the fix, and
[Global Lookup Codegen](Global-Lookup-Codegen.md) for the codegen
comparison. One real, deliberately-left-visible consequence: a coroutine
function can still declare an `est::loop&` first parameter (existing code
that predates the collapse does, in several places) - `promise_type`'s
`Args&...` pack still matches it, it's just silently ignored now instead
of read. Nothing catches a caller passing a `loop&` that isn't actually
`est::current_loop()` here.

`promise_type` doesn't hold its own `est::promise<T>` at all: it builds a
`shared_ptr<future_state<T>>` directly (the same call
`make_promise_future<T>()` makes internally) and talks to it straight
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
`[]() -> future<int> { co_return 42; }` coroutine, for instance, never
touches its loop at all beyond the frame allocation itself: the call
returns an already-`ready()` future directly, no `run_until_idle()`
needed to observe it.

A coroutine's synchronous prefix, up to its first real suspension point,
is conceptually the same as the work an ordinary function does before
returning a future - paying for a heap-allocated resume node plus a full
ready-queue round trip to kick off that prefix, when there's nothing to
actually interleave with, isn't worth what it would buy. Every actual
`co_await` still allocates its own resume node independently, unaffected
by what `initial_suspend()` does - this only changes the coroutine's
*first* slice of work, before it ever suspends. `operator co_await()`
(below) takes the identical "don't wait for something that isn't being
waited for" stance on the *awaiting* side, for the same reason.

One real consequence: a caller of a coroutine-returning function can no
longer assume the `future<T>` it gets back is never already resolved, with
side effects already applied, before it was ever inspected - a plain
function call already carries that same property (nothing stops an
ordinary function from doing arbitrary work before returning), so this
isn't a new kind of risk, just coroutines no longer being exempt from it.

## Why a resume node must be heap-allocated, not frame-embedded

Every awaiter in this codebase (`future_awaiter<T>` below,
`counting_event<Mode>::wait()`'s - `mutex::lock()` doesn't have one of its
own any more, see "`est::mutex`: `lock()` built on top of
`binary_event`" below) follows the same split: the awaiter object itself
stays a coroutine-frame subobject (fine, since nothing reaches back into
it after its own `co_await` expression ends), but `await_suspend()`
allocates a small, *separately heap-allocated* resumption node
(`future_resume_node<T>`, `detail::promise_resume_node<T>`, ...) and registers
that instead of registering the awaiter itself.

That split is required, not just a style choice. `est::loop::run_one()`
calls `node.run()` and then, via a `unique_ptr<detail::ready_node>` guard,
deletes `node` — *after* `run()` has already returned:

```cpp
void run_one(detail::ready_node& node) {
  const std::unique_ptr<detail::ready_node> guard(&node);
  ...
  node.run();
  ...
} // guard's destructor fires here, deleting node through the dynamic type's own vtable slot
```

If `node` were embedded in the very coroutine frame that `run()`'s
`handle_.resume()` call just resumed, resuming the coroutine *past its
own suspension point* would let the compiler reuse that exact frame
storage for whatever the coroutine's later code constructs — its next
awaiter, a local variable — since the two objects' lifetimes don't
overlap. By the time `guard`'s destructor deletes it, that memory could
already hold something else entirely, and calling a virtual function
(the destructor itself) through it would be undefined behavior.

A separately allocated node has its own independent lifetime, entirely
unrelated to the coroutine frame it resumes — `run()`-then-delete is
exactly as safe here as it already is for every other `ready_node` in this
codebase (`concrete_continuation<Fn, U>`, `est:promise`'s `sleep_resume_node`/
`promise_resume_node<T>`, the latter shared with `est:sync.event`). This
is also why `final_suspend()` safely uses plain `std::suspend_never` (the
coroutine frame self-destructs immediately on completion): nothing that
ever resumes the coroutine lives inside the frame being destroyed.

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
    est_loop->>node: run() -> handle.resume()
    Note over coro: resumes here
  end
  coro->>awaiter: await_resume(): get() the value (or rethrow)
```

`await_ready()` returns `future_.ready()` directly - `co_await` on an
already-ready future skips suspension entirely, resuming the rest of the
awaiting coroutine's body immediately, right there on whatever call stack
reached that `co_await`, for the identical reasoning `initial_suspend()`
(above) uses: paying for a `future_resume_node<T>` allocation and a full
ready-queue round trip purely to resume something that was never
actually going to wait for anything isn't worth it. `.then()` registered
on an already-ready future used to always defer through `est::loop`
rather than running inline regardless of what a caller wanted - issue
#65 flagged this as a "subtly different," not obviously intentional,
asymmetry between the two ways of consuming a future. `then_fast()`
(`future_state<T>`'s own doc comment, `future.cppm`; see [Continuation
Node Mechanism](Continuation-Node-Mechanism.md) for the full mechanism)
is the resolution: `then()` keeps deferring by default - a `.then()`
callback runs arbitrary caller code, and `then()` must stay safe for a
chain of any length, which deferring through `est::loop`'s own iterative
drain guarantees regardless of chain length - while `then_fast()` is the
explicit opt-in for a caller who specifically wants the same "resume
right here" behavior `co_await` already gets for free, and knows its own
callback is cheap enough to accept the recursion-depth trade that comes
with it. `future_state<T>::set_continuation()` (called from
`await_suspend()`) still handles the "not yet ready" case exactly as
before - this only changes whether that call, and the node it needs,
happens at all, which is what the diagram's `alt` now shows.

`future_awaiter<T>` needs its resumption node to satisfy
`future_state<T>::set_continuation()`'s signature —
`detail::continuation_node<T>&` — so it gets its own tiny heap-allocated
trampoline, `future_resume_node<T>`, that implements `ready_node::run()`
directly and just resumes (it has no use for `owner_`
(`continuation_node<T>`'s own member) beyond what keeps its parent
`future_state<T>` alive - see [Continuation Node
Mechanism](Continuation-Node-Mechanism.md) for why `run()` lives on each
concrete node rather than behind a shared, separately virtual `invoke()`):

```cpp
template <class T> class future_resume_node final : public continuation_node<T> {
public:
  explicit future_resume_node(std::coroutine_handle<> handle) noexcept : handle_(handle) {}
  void run() final {
    handle_.resume();
  }
  void abandon() noexcept override {
    handle_.destroy();   // see "Abandoned coroutines are destroyed, not leaked" below
  }
  static auto operator new(std::size_t size) -> void*;   // resolves current_allocator()
  static void operator delete(void* ptr, std::size_t size) noexcept;
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
many there are — `.then()` only ever moves instead of copying when its own
node turns out to be the *sole* surviving `shared_ptr<future_state<T>>`
owner (issue #64; see the linked page), which by definition rules out any
sibling continuation left to strand. The rvalue path (`std::move(future).
get()`) *moves* it out instead, and a move doesn't reset `result_` — it
just leaves whatever moved-from state `T` ends up in sitting there
permanently, since nothing else ever touches that variant again.
`await_resume()` above always takes this path.

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
  void set_continuation(continuation_node& node) {
    if (ready()) {
      node.bind_owner(this->shared_from_this());
      current_loop().enqueue_ready(node);
      return;
    }
    waiters_.enqueue(node);
  }
  ```
  So "how many waiters exist when `set_value()`/`complete()` runs" isn't
  "how many will ever exist" — a value moved out (or discarded) right
  then based on that count would break any consumer that shows up later.
  `result_` has to stay in `future_state` for as long as the
  `future_state` itself lives.
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
strings), and `.then()`'s own unwrapped dispatch *does* use exactly this
shape (issue #64; [Continuation Node
Mechanism](Continuation-Node-Mechanism.md#the-two-calling-conventions)) —
but it can do so only because the check and the consumption happen
together, synchronously, inside the same node's own `run()`, checking the
very `shared_ptr<future_state<T>>` (`owner_`) that call is about to read
through: nothing else can be a third party holding a reference the check
doesn't see. `get()`/`await_resume()` can't reuse that trick for the
reason spelled out just above — the object making the "am I the sole
owner" decision (`future<T>::get()`) and the object guaranteed to be
holding a reference at that exact moment (the resume node driving the
resumption) aren't the same `shared_ptr`, so the check would almost never
see what it's actually looking for. `.then()`'s move path sidesteps this
by being both the checker and the consumer in one. `future<T>::clone()`
makes an explicit alias of the same `future_state<T>` available to an
arbitrary caller too, but only for `T = void` or a scalar `T` - see
`future<T>::clone()`'s own doc comment (`future.cppm`) for why every
other `T` would reopen exactly this hazard.

### Composability, concretely

Because `co_await` works uniformly on any `future<T>`, a coroutine can await
another coroutine's result directly:

```cpp
est::future<int> inner(int x) { co_return x * 2; }

est::future<int> outer() {
  const int value = co_await inner(21);
  co_return value + 1;
}
```

or a `then()`-chain future built by ordinary, non-coroutine code:

```cpp
auto [promise, future] = est::make_promise_future<int>();
auto chained = future.then([](int v) { return v + 1; });

est::future<int> coro(est::future<int> fut) {
  const int value = co_await std::move(fut);
  co_return value * 10;
}
coro(std::move(chained));
```

Neither `inner`'s caller nor `chained`'s consumer needs to know or care that
the other side is a coroutine.

### Abandoned coroutines are destroyed, not leaked

Every resumption node's `abandon()` is called exactly once, right before
it's deleted, but only on one of two paths: never after a successful
`run()` (the normal case, described above), but always from
`future_state<T>::~future_state()`/`loop::drain_pending()` (called both by
`~loop()` and by `make_current_loop()`'s own returned guard - see
[Loop and Timers](Loop-And-Timers.md) for why the guard needs to call it
too) draining whatever's left when a `future_state`/`loop` is torn down
without ever completing/draining (the same abandoned-future scenario the
node hierarchy already handles for `then()`'s own continuation nodes).
Freeing only the resumption node
itself in that second case, never the coroutine frame the handle points
to, would permanently leak it -

```cpp
template <class T> class future_resume_node final : public continuation_node<T> {
public:
  void run() final {
    handle_.resume();
  }
  void abandon() noexcept override {
    handle_.destroy();   // never resumed - still fully intact; this is
                          // the only chance to free its frame
  }
  ...
};
```

If `run()` never happened, the coroutine is still exactly where
`await_suspend()` left it - fully intact, suspended, never touched -
so destroying it in `abandon()` is both safe and necessary. If `run()`
*did* happen, `abandon()` is never called at all, since the loop deletes
such a node directly instead (`loop::run_one()`'s own guard) - which matters
because touching `handle_` at that point would be wrong: the coroutine
either already self-destroyed (`promise_type::final_suspend()`'s
`std::suspend_never` - `handle_` is now dangling, so even calling
`.done()` on it would be a use-after-free) or suspended again on
something else entirely, which now owns resuming (and eventually
destroying) it. Splitting this into `abandon()` (only called on the
not-run path) rather than one `destroy()` told whether it ever ran
(`ready_node::abandon()`'s own doc comment, est:loop) means every node's
own logic never has to branch on that itself - the call site
already knows statically which situation it's in.

## `est::counting_event<Mode>` becomes awaitable

`est::counting_event<Mode>` is bookkeeping-only (one `int` count, an
intrusive waiter list — see [Architecture](Architecture.md)), because
nothing is concurrent enough to need real protection. Coroutines
introduce a genuine, if still single-threaded, race: two coroutines each
doing "read some shared structure, `co_await` something, write it back"
can interleave *at that suspension point* and corrupt it — a
cooperative-scheduling race, not interrupt-context reentrancy. `wait()`
is this framework's answer to that for anything shaped like a semaphore
or an event; `est::mutex` (below) is nothing more than one built on top
of it.

`wait()` returns a plain `future<void>`, built the same way every other
producer in this codebase is - no bespoke awaiter type. Every producer
here - `then()`, `future_awaiter<T>`, `promise_type::initial_suspend()` -
pays a fixed allocation cost specifically so a caller never has to
wonder whether a given `future<T>` might already be resolved, with side
effects already applied, before it was ever inspected; a bespoke fast
path for `wait()` alone would break that guarantee. `try_wait()` is
the synchronous half of that fast path, split out on its own so a caller
that doesn't want a `future<void>` at all (`est::mutex::lock()`, below)
can reuse it directly instead of building and immediately discarding one:

```cpp
[[nodiscard]] auto try_wait() noexcept -> bool {
  if (count_ <= 0) {
    return false;
  }
  if constexpr (Mode == EventResetMode::automatic) {
    --count_;
  }
  return true;
}

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

`counting_event<Mode>` holds no `loop&` of its own (nor does
`future_state<T>` any more) - and `wait()` doesn't even need
`current_loop()` itself here, only `current_allocator()`: the fast path
completes the promise inline, and the slow path only enqueues into this
event's own `waiters_`, never onto a loop's ready-queue directly (that
happens later, from `set()`, which does need `current_loop()`). See
[Loop and Timers](Loop-And-Timers.md) for what "resolve fresh, don't
cache" costs and the hazard it introduces.

`co_await event.wait();` works because `future<T>` is awaitable from any
coroutine (`operator co_await()`, above) - and since
`future_awaiter<T>::await_ready()` checks `future_.ready()` directly (see
that section above), an uncontended `co_await event.wait()` resumes
inline: no `future_resume_node<T>` allocated and no loop round-trip,
since `await_suspend()` never runs when `await_ready()` already returns
`true`. The one allocation this fast path still pays is the
`future_state<void>` control block itself, from
`make_ready_future<void>()` - `wait()` builds one unconditionally, fast
path or not, since it has no way to hand back a `future<void>` without
one. A contended `wait()` additionally allocates the `promise_resume_node<void>`
queued in `waiters_`.

`wait()` is also no longer awaitable-*only*: since it returns a plain
`future<void>`, it can be used from ordinary, non-coroutine code too
(polled via `ready()`/`get()`, or chained with `then()`), not just via
`co_await`. `event.wait().then_fast(fn)` gets the uncontended case the
identical "resume right here, no loop round trip" treatment
`co_await event.wait()` already does (`then_fast()` - [Continuation Node
Mechanism](Continuation-Node-Mechanism.md)); plain `then()` still defers
even here, since it has no way to know a given `fn` is cheap enough to
run inline.

`set(n)` hands available units directly to queued waiters (via
`loop.enqueue_ready()`, never completing their promises inline here — the
same "always deferred through the loop" reasoning as everywhere else,
avoiding unbounded call-stack growth for a chain of coroutines that each
wait/set in turn) rather than only ever growing the count first:

```cpp
auto set(int n = 1) -> int {
  check(n > 0, "counting_event::set(n) requires n > 0");
  const int actual_n = std::min(n, max_count_ - count_);
  if (actual_n <= 0) {
    return 0;
  }
  count_ += actual_n;
  if (!waiters_.empty()) {
    auto& loop_ref = current_loop();
    if constexpr (Mode == EventResetMode::automatic) {
      for (int i = 0; i < actual_n; ++i) {
        auto* waiter = waiters_.dequeue();
        if (waiter == nullptr) {
          break;
        }
        --count_;
        loop_ref.enqueue_ready(*waiter);   // ownership passes directly - no window where it reads free
      }
    } else {
      waiters_.drain([&loop_ref](detail::ready_node& node) { loop_ref.enqueue_ready(node); });
    }
  }
  return actual_n;
}
```

Waiters resume in `est::intrusive_list`'s documented FIFO order — the
first-queued waiter is the first one handed a unit once one's available.

### `detail::promise_resume_node<T>`: completing on abandonment, not just dropping

`~counting_event()` drains `waiters_` the same way `future_state<T>`'s
and `est::loop`'s own destructors drain theirs — an event destroyed with
a waiter still queued on `wait()` must not just leak it.

`detail::promise_resume_node<T>` (`est::detail`, defined in `est:promise`
rather than nested inside `counting_event<Mode>` - its `run()`/
`abandon()` never touch `Mode` or anything else about the
`counting_event` that enqueued it, so nesting it would only generate an
identical type once per `Mode` instantiation for no reason; it's the same
node `yield_execution()` uses, below, always as `promise_resume_node<void>`
- issue #77 collapsed what used to be two byte-identical types, one per
call site, into this one shared, templated class) holds a `promise<T>`,
not a `coroutine_handle<>` directly, so naively deallocating an
abandoned node (matching the "broken promise, the future simply never
becomes ready" contract every other dropped `est::promise<T>` in this
codebase otherwise has by default) would reopen a real leak: a coroutine
doing `co_await event.wait();` holds the resulting `future<void>` as a
temporary spilled into its own frame across the suspension - the *only*
other reference to that `future_state<void>`, besides the node in
`counting_event<Mode>::waiters_`. Drop the node's promise silently, and
the future_state is left forever "not yet ready," kept alive solely by
the very coroutine frame that can only ever be freed by that
future_state eventually completing. Neither side can free the other
first - not a `shared_ptr` cycle in the strict sense (no object holds a
`shared_ptr` back to the thing keeping it alive), but a practical one:
both stay allocated forever.

`abandon()` breaks that by actually completing the promise, with
`detail::abandoned_exception` (`est:loop` - the one shared, message-less
exception type every `abandon()` override in this codebase that needs to
complete something, rather than just deallocate, throws; nothing here
ever inspects `what()` to tell one abandonment apart from another, so
there was nothing for a per-call-site message to actually communicate),
before the node is deleted:

```cpp
void abandon() noexcept final {
  promise_.set_exception(std::make_exception_ptr(abandoned_exception()));
}
```

Completing the promise drains the future_state's own pending continuation
(the coroutine's `future_resume_node<T>`, or a `.then()`-registered
continuation like `est::mutex::lock()`'s, below) onto `est::loop`'s
`ready_` queue - where it's either genuinely resumed (if the loop
outlives this event and keeps running - the coroutine then observes the
exception exactly like any other failure propagating across `co_await`,
per "an exception in the awaited future propagates across `co_await`"
above) or safely destroyed, never run, by `est::loop`'s own
destructor-time drain - either way, the frame is no longer stranded.
Splitting the not-run path into its own `abandon()` (`est:future`'s own
`future_resume_node<T>` overrides the identical method - `ready_node::
abandon()`'s own doc comment, est:loop) is what stops this from
double-completing an already-successfully-completed promise: `abandon()`
is only ever called on a node the loop drains *without* first calling
`run()` on it; a node the loop does `run()` is deleted directly instead,
no `abandon()` call at all (see [Continuation Node
Mechanism](Continuation-Node-Mechanism.md)).

`counting_event<Mode>` also takes a `max_count` (constructor argument,
defaulted to an effectively-unbounded value): `set(n)` saturates at it -
actually adding `min(n, max_count - count())` - rather than growing the
count without limit, the same way `std::counting_semaphore<LeastMaxValue>`
bounds `release()`, except saturating instead of the standard's own
undefined-behavior-on-overflow contract. `set(n)` returns that actual
amount (`0` for a no-op call, up to `n` otherwise) rather than `void`, so
a caller of the base class - unlike `binary_event`/`one_shot_event`,
which exist precisely so their own callers never have to check - can
still tell whether a `set(n)` call was silently capped.
`one_shot_event::set()` returns the same shape (`1` for the call that
actually signals, `0` for a redundant one). `binary_event<Mode>` (count
clamped to `{0, 1}`, the classic Win32 event object) is nothing more than
a `counting_event<Mode>` constructed with `max_count = 1` - not a separate
implementation, and no override of `set()` needed: the base class's own
saturation at `max_count` already makes `set()` idempotent once signaled,
with no code of `binary_event`'s own - `max_count` alone enforces the
`{0, 1}` clamp regardless of which overload a caller reaches.

`one_shot_event<Mode>` (`set()` at most once, ever) still declares its own
no-argument `set()`, hiding the inherited `set(int n)` from unqualified
lookup - not a virtual override, since nothing in this hierarchy ever
needs runtime dispatch - but for a different reason than `binary_event`
originally had: `one_shot_event` tracks "has `set()` ever been called" in
its own `has_been_set_` flag (`signaled()` alone can't tell "never set()"
apart from "set() once, already consumed" for `Mode::automatic`), and a
caller reaching the inherited `set(int n)` directly would bypass that
flag entirely. `one_shot_event::reset()` is `= delete`d the same way,
turning "don't call this" from a documented-but-unchecked precondition
into a compile error.

## `est::mutex`: `lock()` built on top of `binary_event<automatic>`

`est::mutex` used to be its own bespoke implementation of exactly the
pieces `counting_event<Mode>` (above) already has: an intrusive waiter
list, a resume node, `abandon()` completing an abandoned waiter with an
exception. Issue #67 removed the duplication: `mutex` now holds a single
`est::binary_event<EventResetMode::automatic>` member and nothing else -
"unlocked" is exactly "one unit available," "locked" is "no unit
available," and automatic mode's own "hand the freed unit directly to
the next queued waiter, never reading as free in between" behavior
(`set()`'s own listing above) is already exactly the handoff semantics a
mutex needs. `mutex`'s own constructor calls `event_.set()` once to
start at "available" (`binary_event`'s own default starting count is
zero, "not yet signaled" - see its own doc comment, `est/src/sync/
event.cppm`); every other method - `locked()`, `has_waiters()`, `~mutex()`
- forwards straight to `event_`'s own `count() == 0`, `has_waiters()`,
and (implicitly, as a subobject) destructor.

`lock()` also merged what used to be two separate methods, `lock()` (a
plain `future<void>`) and `acquire()` (a `future<lock_guard>`), into one
(issue #66): a lock without the RAII guard to release it has no use in
this codebase - every call site immediately wanted the guard - so there
is nothing a second, lower-level pair would add besides a way to forget
to `unlock()`. `unlock()` itself is now `private`, reachable only through
`lock_guard`'s own destructor/move-assignment (a nested class already
shares its enclosing class's access) - the *only* way left to release a
lock `lock()` returned:

```cpp
[[nodiscard]] auto lock() -> future<lock_guard> {
  if (event_.try_wait()) {
    return make_ready_future<lock_guard>(*this);
  }
  return event_.wait().then([this] { return lock_guard(*this); });
}

private:
void unlock() noexcept { event_.set(); }
binary_event<EventResetMode::automatic> event_;
```

The fast (uncontended) path calls `try_wait()` directly rather than
`event_.wait()` - reusing `wait()` itself here would mean discarding the
`future<void>` it always builds (even on its own fast path) just to
build a second, different one (`future<lock_guard>`) in its place, an
extra wasted allocation on every uncontended `lock()` call. `try_wait()`
being a plain, non-allocating `bool` check is exactly what avoids that;
see the previous section for why it exists as its own method on
`counting_event<Mode>` rather than being inlined into `wait()` alone.
This keeps `lock()`'s fast path exactly as cheap as before this refactor
(see [Allocation Patterns](Allocation-Patterns.md)): one allocation, the
returned `future_state<lock_guard>` itself, same as `make_ready_future<T>()`
pays for any `T`.

The slow (contended) path is where the simplification actually costs
something: `event_.wait().then(...)` allocates `event_.wait()`'s own
`future_state<void>` + `promise_resume_node<void>` (exactly what the old,
hand-written `acquire_resume_node` also needed), *plus* `.then()`'s own
downstream `future_state<lock_guard>` + `concrete_continuation<Fn,
lock_guard>` node (see [Continuation Node
Mechanism](Continuation-Node-Mechanism.md)) to turn the `future<void>`
into the `future<lock_guard>` `lock()` actually returns - four
allocations where the old `acquire_resume_node` needed two. A deliberate
trade: not re-implementing the waiter queue, resume node, and
abandonment-completion machinery a second time, in exchange for two
extra allocations only a *contended* `lock()` call ever pays. See
`docs/PLAN.md`'s "Issue #66 & #67" entry for the full reasoning.

**The `.then()` callback still needs a live `mutex&` when it eventually
runs** - `[this] { return lock_guard(*this); }` captures `this` (a
`mutex*`), and that continuation only actually runs once `event_.wait()`'s
future completes and `est::loop` drains the `.then()`-registered node,
some time after `unlock()`/`event_.set()` enqueued it. This is the exact
same lifetime precondition the old `acquire_resume_node::run()` carried
(needing a live `mutex&` to construct the `lock_guard` it completed
with) - moved to a different node by this refactor, not newly introduced
by it, and not fixed by it either: **issue #46 (still open) tracks
closing this gap**; until then, the precondition is unchanged from
before this refactor - whichever loop drains a mutex's deferred
completions must outlive every mutex whose waiters it enqueues, and a
caller must not let that loop keep running past a mutex's destruction
while a waiter is still queued on it.

## Cancellation: `stop_token` vs. abandonment

Abandonment (`detail::abandoned_exception`, above) and `est::stop_token`/
`est::stop_source` (`est:sync.stop_token`) solve two different problems
that both end up completing a pending `future<T>` with an exception -
easy to conflate, since both funnel through the same `set_exception()`
channel, but they answer different questions:

- **Abandonment answers "will anything ever resume this again?"** It only
  ever fires from a destructor-time drain (`future_state<T>::~future_state()`,
  `loop::drain_pending()`) - the *owner* of whatever a coroutine/waiter was
  suspended on going away. There is no way to abandon something on demand;
  it's purely a teardown-time cleanup so a suspended frame doesn't leak
  forever with nothing left able to resume it.
- **`stop_token` answers "please stop waiting, even though the thing you're
  waiting on is still perfectly alive and might still complete."** A
  `stop_source`/`stop_token` pair (built directly on `future<void>` -
  `stop_token::stopped()` is a `future<void>` any number of independent
  consumers can `co_await`/`then_fast()` via `clone()`) is an explicit,
  on-demand signal a caller controls, unrelated to any object's lifetime.

`est::with_stop<T>(future<T> operation, const stop_token& token)`
(`est:with_stop`) races `operation` against `token.stopped()` and
resolves the moment either one does - the returned `future<T>` fails with
`est::operation_cancelled` if the token wins. Its honestly-scoped
limitation: it only stops the *caller* from waiting further; `operation`
itself keeps running in the background until it completes on its own
(matching `when_any()`/`when_all()`/`when_any_succeeds()`'s own already-
accepted "never cancels the ones that haven't finished yet" limitation -
see [Architecture](Architecture.md)). Eagerly freeing an arbitrary
suspended coroutine or queued `mutex::lock()`/`counting_event::wait()`
waiter would need `intrusive_list<T>::remove()` from the middle, which
doesn't exist (`enqueue`/`dequeue`/`drain` only) - out of scope for now.

The one case where cancellation *is* genuinely eager - not just "stop
watching" - is a timed wait: `est::sleep_for(delay, const stop_token&)`/
`est::sleep_until(deadline, const stop_token&)` (`est:with_stop`) build on
`loop::cancel_timer(timer_id)`, which pulls a still-pending
`detail::sleep_resume_node` out of `loop`'s timer queue early and
completes it via the same `abandon()` path `drain_pending()` uses at
teardown - real, immediate reclamation, not a flag checked later. This
works only because `loop` already tracks pending timers by id
(`schedule_timer()` returns one); no such id exists for a queued
`intrusive_list` waiter.
