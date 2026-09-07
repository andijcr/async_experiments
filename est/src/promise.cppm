export module est:promise;

import std;
import :future;
import :loop;
import :platform;
import :util.current_loop;
import :util.shared_ptr;

export namespace est {

// Producer handle: a thin, move-only view over a future_state<T>.
template <class T> class promise {
public:
  // Same as future_state<T>::stored_t: T itself, except when T is void,
  // where it's a stand-in tag type instead - const T&/T&& below would be
  // "reference to void", ill-formed, and (unlike a requires-clause,
  // which only gates overload resolution) a parameter type is elaborated
  // as soon as promise<T> itself is instantiated, requires-clause or not.
  using stored_t = future_state<T>::stored_t;

  explicit promise(shared_ptr<future_state<T>> state) noexcept : state_(std::move(state)) {}
  promise(const promise&) = delete;
  auto operator=(const promise&) -> promise& = delete;
  promise(promise&&) noexcept = default;
  auto operator=(promise&&) noexcept -> promise& = default;
  ~promise() = default;

  void set_value()
    requires std::is_void_v<T>
  {
    state_->set_value();
  }

  void set_value(const stored_t& value)
    requires(!std::is_void_v<T>)
  {
    state_->set_value(value);
  }

  void set_value(stored_t&& value)
    requires(!std::is_void_v<T>)
  {
    state_->set_value(std::move(value));
  }

  void set_exception(std::exception_ptr exception) { state_->set_exception(std::move(exception)); }

private:
  shared_ptr<future_state<T>> state_;
};

// Constructs a fresh future_state<T> against `loop_ref` (using its
// allocator - see future_state<T>'s own doc comment on why it holds a
// loop& instead of an allocator directly, docs/PLAN.md, M3) and returns
// the promise/future pair that share it. This is the only way a
// future_state gets created - promise<T>/future<T> only otherwise exist
// as the result of a move.
template <class T> auto make_promise_future(loop& loop_ref) -> std::pair<promise<T>, future<T>> {
  auto state = shared_ptr<future_state<T>>::make(loop_ref.allocator(), loop_ref);
  auto state_for_future = state; // copy bumps the ref count from 1 to 2
  return {promise<T>(std::move(state)), future<T>(std::move(state_for_future))};
}

// Issue #30: sugar over the overload above using est::current_loop()
// (est:util.current_loop) instead of a caller-supplied loop& - for a
// caller that doesn't want to thread a loop& through by hand and is
// content relying on whichever loop is current.
template <class T> auto make_promise_future() -> std::pair<promise<T>, future<T>> {
  return make_promise_future<T>(current_loop());
}

} // namespace est

namespace est::detail {

// The node behind sleep_for()/sleep_until() (below): holds a
// promise<void> directly rather than a generic Fn - an earlier version
// (concrete_timer_node<Fn>, wrapping a closure that itself captured the
// promise) couldn't complete that promise on abandonment (destroy()
// called without fire() ever having run), because a type-erased Fn
// gives destroy() no way to know it's holding a promise at all, let
// alone call set_exception() on it. That was a real, latent bug (issue
// #50): a coroutine doing `co_await sleep_for(loop, 10s);`, abandoned
// when its loop is destroyed before the timer ever fires, would leak
// its own frame forever - the exact shape of hazard `ran`-guarded
// exception-completion already fixed for mutex::lock_resume_node/
// acquire_resume_node (PR #37 review) and detail::yield_resume_node
// below, for the identical reason: the awaiting coroutine holds the
// only other reference to this promise's future_state<void> (the
// future<void> temporary co_await awaits is spilled into the
// coroutine's own frame across the suspension), so silently dropping
// the promise instead of completing it would strand that frame with
// nothing left to free it.
class sleep_resume_node final : public timer_node {
public:
  explicit sleep_resume_node(promise<void> prom) noexcept : promise_(std::move(prom)) {}

  void fire() override { promise_.set_value(); }

  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept override {
    if (!ran) {
      promise_.set_exception(std::make_exception_ptr(
          std::runtime_error("loop destroyed while sleep_for()/sleep_until() was pending")));
    }
    allocator.delete_object(this);
  }

private:
  promise<void> promise_;
};

// The node behind yield_execution() (below): a plain ready_node holding
// a promise<void>, handed straight to loop_ref.enqueue_ready() instead
// of routed through schedule_timer() - yield_execution() has no deadline
// to track, so the timer_queue heap insert, pending_timers_'s own linear
// search+erase on fire, and the platform::sleep_until() call run_impl()
// makes before it ever checks pending_timers_ are all pure overhead for
// something that only ever needs "run after whatever's already ready."
// est::intrusive_list<T>'s FIFO order (not this class's original policy -
// see its own doc comment) is what makes handing this straight to
// ready_ safe: enqueue_ready() appends at the tail, so everything
// already queued when yield_execution() was called runs first - the
// same trick under the old LIFO policy would have cut this node in
// line ahead of everything else instead.
//
// run()/destroy() completing the promise with an exception on
// abandonment (rather than silently dropping it, the "broken promise,
// future simply never becomes ready" default every other est::promise<T>
// in this codebase otherwise has) matters for the identical reason
// sleep_resume_node's own doc comment (just above) and
// mutex::lock_resume_node's own doc comment (est:sync.mutex) both give.
class yield_resume_node final : public ready_node {
public:
  explicit yield_resume_node(promise<void> prom) noexcept : promise_(std::move(prom)) {}

  void run() final { promise_.set_value(); }

  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept final {
    if (!ran) {
      promise_.set_exception(std::make_exception_ptr(
          std::runtime_error("loop destroyed while yield_execution() was pending")));
    }
    allocator.delete_object(this);
  }

private:
  promise<void> promise_;
};

} // namespace est::detail

export namespace est {

// Returns a future<void> that becomes ready once `deadline` passes,
// driven by `loop_ref`'s own timer_queue (est:loop) - the est::loop <->
// est::future/est::promise bridge that keeps :loop itself free of any
// dependency on est::future/est::promise (see :loop's own top comment on
// why: :loop is the lower-level partition future_state<T> itself depends
// on, so it cannot depend back on :future/:promise).
[[nodiscard]] inline auto sleep_until(loop& loop_ref, loop::clock::time_point deadline)
    -> future<void> {
  auto [prom, fut] = make_promise_future<void>(loop_ref);
  auto* node = loop_ref.allocator().template new_object<detail::sleep_resume_node>(std::move(prom));
  loop_ref.schedule_timer(*node, deadline);
  return std::move(fut);
}

// Issue #30: sugar over the overload above using est::current_loop()
// instead of a caller-supplied loop&.
[[nodiscard]] inline auto sleep_until(loop::clock::time_point deadline) -> future<void> {
  return sleep_until(current_loop(), deadline);
}

// Returns a future<void> that becomes ready once `delay` elapses from
// now (est::platform::instance().now(), the same clock est::timer_queue
// itself is built on, docs/PLAN.md M1) - sugar over sleep_until() above.
[[nodiscard]] inline auto sleep_for(loop& loop_ref, loop::clock::duration delay) -> future<void> {
  return sleep_until(loop_ref, platform::instance().now() + delay);
}

// Issue #30: sugar over the overload above using est::current_loop()
// instead of a caller-supplied loop&.
[[nodiscard]] inline auto sleep_for(loop::clock::duration delay) -> future<void> {
  return sleep_for(current_loop(), delay);
}

// Issue #45: gives `loop_ref` the opportunity to run whatever else is
// already ready before the calling coroutine resumes - `co_await
// yield_execution(loop);` inside a loop that would otherwise monopolize
// the ready-queue with back-to-back synchronous resumes (every
// `co_await` on an already-ready future skips suspension entirely,
// est:future's own `future_awaiter<T>::await_ready()`) lets other
// pending work interleave instead.
//
// Originally sugar over sleep_for(loop_ref, 0) - a zero-duration timer
// lands in pending_timers_, only reached once drain_ready() has fully
// emptied ready_ first, giving exactly the right ordering "for free."
// Replaced with a direct detail::yield_resume_node once
// est::intrusive_list<T> became FIFO (PR #49): re-entering ready_
// directly is now just as correctly ordered, without paying for a
// timer_queue heap insert/pop, pending_timers_'s own linear search+erase
// on fire, or the platform::sleep_until() call run_impl() makes before
// it ever checks pending_timers_ - none of which yield_execution() ever
// needed, having no real deadline to track.
[[nodiscard]] inline auto yield_execution(loop& loop_ref) -> future<void> {
  auto [prom, fut] = make_promise_future<void>(loop_ref);
  auto* node = loop_ref.allocator().template new_object<detail::yield_resume_node>(std::move(prom));
  loop_ref.enqueue_ready(*node);
  return std::move(fut);
}

// Issue #30: sugar over the overload above using est::current_loop()
// instead of a caller-supplied loop&.
[[nodiscard]] inline auto yield_execution() -> future<void> {
  return yield_execution(current_loop());
}

} // namespace est
