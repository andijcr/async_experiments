# Allocation patterns

Everything owned in `est` — a `future_state<T>`'s control block, a
continuation node, a timer's callback node — goes through one explicit
`std::pmr::polymorphic_allocator<std::byte>`: the one an `est::loop` was
built with (`loop.allocator()`). There's no hidden global `new`/`delete`
anywhere in the hot path, and no type-erased wrapper (a `std::function`, a
`std::move_only_function`) that might allocate through its own internal
strategy instead of the caller's chosen memory resource. This page traces
exactly what gets allocated, when, and by whom — the answer to "what does a
complex `.then()` chain actually cost."

## The two allocation primitives

1. **`est::shared_ptr<T>::make(allocator, args...)`** — *one* allocation for
   a `control_block` that combines the ref count, the allocator, and `T`
   itself. This is what every `future_state<T>` is built through
   (`make_promise_future()`, and every `then()` call's downstream state).
   Unlike `std::shared_ptr<T>` built via `new T` then wrapped, there's never
   a second, separate control-block allocation to worry about — `make()` is
   already the `make_shared`-equivalent path, always.
2. **`allocator.new_object<Concrete>(args...)`** — a *direct*, un-shared
   allocation for a continuation or timer node (`concrete_continuation<Fn,
   U>`, `est:promise`'s `sleep_resume_node`/`yield_resume_node`). These
   are never wrapped in a `shared_ptr` — a node has exactly one owner at a
   time (first the `future_state` it's pending on, then the loop's
   ready-queue or pending-timer list), so plain ownership-by-pointer plus
   an explicit virtual `destroy(allocator, ran)` call is enough; see
   [Continuation Node Mechanism](Continuation-Node-Mechanism.md) for why
   `destroy()` has to be virtual at all (deallocating the *actual* derived
   type through a `ready_node*`/`timer_node*` base pointer).

## Allocation cost per operation

| Operation | Allocations | What they are |
|---|---|---|
| `make_promise_future<T>(loop)` | **1** | `future_state<T>`'s control block |
| `future<T>::then(fn)` (plain, non-flattening) | **2** | the downstream `future_state<U>`'s control block, plus the `concrete_continuation<Fn, U>` node |
| `est::sleep_for()` / `sleep_until()` | **2** | `future_state<void>`'s control block, plus the `sleep_resume_node` node |
| `.then(fn)` where `fn` returns a `future<V>` (flattening) | **2 up front + 1 more when it runs** | the usual 2 for the visible registration, plus 1 more, *invisible to the caller*, for `detail::flatten_forwarder<V>`'s forwarding node — see below |

A plain chain of `N` `.then()` calls off one `make_promise_future` therefore
costs **`1 + 2N`** allocations, full stop — regardless of how deep the chain
is, each link is exactly 2 allocations, known statically at the call site.

## Flattening costs one extra allocation

[Continuation Node Mechanism](Continuation-Node-Mechanism.md#flattening-is-not-a-special-case)
covers *why*: a `.then()` callback returning `future<V>` doesn't get special
node-hierarchy treatment. `fulfill()` allocates a `detail::flatten_forwarder<U>`
node directly on the inner future's own `future_state<U>`, reached through
`future<U>`'s private `state_` member (`future<T>` friends every
`future_state<X>` instantiation for exactly this one internal call site):

```cpp
auto* node = result.state_->allocator().template new_object<detail::flatten_forwarder<U>>(
    downstream_);
result.state_->set_continuation(*node, result.state_);
```

`flatten_forwarder<T>` (issue #25) is templated on the inner value type
alone — no `Fn`, no closure, no `future<T>` view at all, no
wrapped/unwrapped dispatch — and every flattening
`.then()` at the same inner type reuses the same instantiation. Two earlier,
now-removed designs paid more for the same one call site: first a plain
`.then()` call, which worked (the discarded `future<void>` it returned was
never wrong, just wasted) but paid for a second, throwaway
`future_state<void>` plus a full `concrete_continuation` node every single
time; then a lower-level `on_ready()`/`raw_continuation<Fn>` pair that
dropped the throwaway `future_state<void>` but still minted a fresh node
(and closure) type per `(T, Fn, U)` call site, and had to be public on both
`future_state<T>` and `future<T>` for `fulfill()` to reach — even though
`fulfill()` was its only legitimate caller.

## Worked example: a three-link chain

```cpp
est::loop loop;
auto [promise, future] = est::make_promise_future<int>(loop);   // alloc #1: FS0 = future_state<int>

auto chained = future
    .then([](int v) { return v * 2; })                          // alloc #2, #3: FS1 + Node_a
    .then([](int v) { return v + 1; })                          // alloc #4, #5: FS2 + Node_b
    .then([](int v) { return std::to_string(v); });             // alloc #6, #7: FS3 (future_state<string>) + Node_c

promise.set_value(10);
loop.run_until_idle();
```

Seven allocations total (`1 + 2×3`), every one of them known at the three
`.then()` call sites — nothing here depends on runtime values. The object
graph right after registration, before `set_value()` runs anything:

```mermaid
graph LR
  promise -->|shared_ptr| FS0["FS0: future_state&lt;int&gt;<br/>waiters_: [Node_a]"]
  future -->|shared_ptr| FS0
  Node_a -->|downstream_| FS1["FS1: future_state&lt;int&gt;<br/>waiters_: [Node_b]"]
  Node_b -->|downstream_| FS2["FS2: future_state&lt;int&gt;<br/>waiters_: [Node_c]"]
  Node_c -->|downstream_| FS3["FS3: future_state&lt;string&gt;"]
  chained -->|shared_ptr| FS3
```

Note `Node_a`/`Node_b`/`Node_c` are **not** reachable via `shared_ptr` from
anywhere in this diagram yet — they're each owned by pointer, sitting inside
their *parent* `future_state`'s own `waiters_` list (`Node_a` inside `FS0`,
`Node_b` inside `FS1`, `Node_c` inside `FS2`). Only once a node is actually
handed to the loop's ready-queue does it acquire a `shared_ptr` back to its
owner (`bind_owner()` — see
[Continuation Node Mechanism](Continuation-Node-Mechanism.md#the-bind_owner-subtlety)).

### What `promise.set_value(10)` + `run_until_idle()` do to that graph

1. `FS0.set_value(10, self)` → `complete(self)` drains `FS0.waiters_`, calls
   `Node_a.bind_owner(self)` (`self` being the same `shared_ptr<FS0>`
   `promise`/`future` already held, threaded through rather than
   manufactured internally — issue #26), hands `Node_a` to
   `loop.enqueue_ready()`. `FS0` now has two owners again: the original
   `promise`/`future` handles (if still alive) *and* `Node_a` itself.
2. `loop.run_until_idle()` dequeues `Node_a`, runs it: `fn_(10)` → `20`,
   reported into `FS1` via `FS1.set_value(20)`. That in turn drains `FS1`'s
   own `waiters_`, `bind_owner()`s and enqueues `Node_b` — which the *same*
   drain loop picks up next (the ready-queue is checked fresh every
   iteration, so a continuation completing another continuation cascades
   within one `run_until_idle()` call, not one per link).
3. After `Node_a.run()` returns, its `destroy_guard` deallocates it —
   `Node_a` is gone, and with it the last strong reference `FS0` might have
   needed beyond the caller's own `promise`/`future` handles (if those were
   already dropped, `FS0` is destroyed here too).
4. The same pattern repeats for `Node_b` → `FS2.set_value(21)` → `Node_c`
   runs → `FS3.set_value("21")` → `Node_c` destroyed.
5. `chained.get()` reads `"21"` out of `FS3`, which is kept alive by the
   `chained` handle itself.

By the time `run_until_idle()` returns, every node has been allocated
*and* freed within that one call — the only things still alive are whatever
`future`/`promise` handles the caller is still holding (`chained`, and
`promise`/`future` if not moved-from).

## Dropping handles early doesn't shrink a chain's memory footprint

A caller dropping the `future<U>` a `.then()` call returned does **not**
free the corresponding node or `future_state` early. The node's
`downstream_` member is itself a `shared_ptr<future_state<U>>` — as long as
the node exists (pending, or queued on the loop), it keeps its downstream
alive regardless of whether the caller kept the `future<U>` handle at all.
This is deliberate: a `.then()` chain runs to completion once started,
independent of whether anyone is still watching each intermediate step —
consistent with `future_state<T>`'s own doc comment on view semantics
(dropping a `future` doesn't destroy the shared state if something else
still references it).

## Verifying this in practice: `counting_resource`

Every allocation-sensitive test in this codebase (`future_tests.cpp`,
`loop_tests.cpp`, `shared_ptr_tests.cpp`) uses the same small helper — a
`std::pmr::memory_resource` wrapper that counts `allocate()`/`deallocate()`
calls:

```cpp
class counting_resource : public std::pmr::memory_resource {
public:
  int allocations = 0;
  int deallocations = 0;
private:
  auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
    ++allocations;
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }
  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
    ++deallocations;
    std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
  }
  // ...
};
```

Passed into `est::loop{&resource}` (which threads it through every
`future_state`/node it's responsible for), a test then asserts
`resource.allocations == resource.deallocations` after the scope holding
everything ends — the only practical way to catch a leaked or wrongly-sized
node in a unit test without a sanitizer. Because *every* allocation in a
chain flows through the one loop's allocator, this single counter catches a
leak anywhere in an arbitrarily deep or flattened chain, not just at the
top level.

The same property is what would let a real caller plug in an arena or pool
`memory_resource` for a whole `est::loop` and have every `future_state`,
every continuation node, and every timer node in every chain built against
it serviced from that one resource — nothing in the design routes any of
this through the global default resource unless a caller explicitly asks for
that as the loop's own allocator.
