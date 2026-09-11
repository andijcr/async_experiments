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

} // namespace est

namespace est::detail {

// Constructs a fresh future_state<T> using `allocator` and returns the
// promise/future pair that share it. This is the only way a future_state
// gets created - promise<T>/future<T> only otherwise exist as the result
// of a move. Not exported: every public entry point (make_promise_future()
// and the producer functions below) resolves est::current_loop() exactly
// once itself and forwards its allocator here, rather than exposing a
// second, explicit-loop&-taking public constructor alongside the
// current-loop one - see docs/wiki/Loop-And-Timers.md for why only
// current_loop() is the public story now.
template <class T>
auto make_promise_future_impl(std::pmr::polymorphic_allocator<std::byte> allocator)
    -> std::pair<promise<T>, future<T>> {
  auto state = shared_ptr<future_state<T>>::make(allocator);
  auto state_for_future = state; // copy bumps the ref count from 1 to 2
  return {promise<T>(std::move(state)), future<T>(std::move(state_for_future))};
}

} // namespace est::detail

export namespace est {

// Builds a fresh promise<T>/future<T> pair against est::current_loop().
template <class T> auto make_promise_future() -> std::pair<promise<T>, future<T>> {
  return detail::make_promise_future_impl<T>(current_loop().allocator());
}

} // namespace est

namespace est::detail {

// The node behind sleep_for()/sleep_until() (below): holds a
// promise<void> directly rather than a generic Fn, so destroy() can
// complete that promise on abandonment (destroy() called without fire()
// ever having run) - a type-erased Fn would give destroy() no way to
// know it's holding a promise at all, let alone call set_exception() on
// it. Without this, a coroutine doing `co_await sleep_for(loop, 10s);`,
// abandoned when its loop is destroyed before the timer ever fires,
// would leak its own frame forever - the same `ran`-guarded
// exception-completion mutex::lock_resume_node/acquire_resume_node
// (est:sync.mutex) and detail::yield_resume_node below also rely on, for
// the identical reason: the awaiting coroutine holds the only other
// reference to this promise's future_state<void> (the future<void>
// temporary co_await awaits is spilled into the coroutine's own frame
// across the suspension), so silently dropping the promise instead of
// completing it would strand that frame with nothing left to free it.
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
// est::intrusive_list<T>'s FIFO order (see its own doc comment) is what
// makes handing this straight to ready_ safe: enqueue_ready() appends at
// the tail, so everything already queued when yield_execution() was
// called runs first.
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
// driven by est::current_loop()'s own timer_queue (est:loop) - the
// est::loop <-> est::future/est::promise bridge that keeps :loop itself
// free of any dependency on est::future/est::promise (see :loop's own
// top comment on why: :loop is the lower-level partition future_state<T>
// itself depends on, so it cannot depend back on :future/:promise).
[[nodiscard]] inline auto sleep_until(loop::clock::time_point deadline) -> future<void> {
  auto& loop_ref = current_loop();
  auto [prom, fut] = detail::make_promise_future_impl<void>(loop_ref.allocator());
  auto* node = loop_ref.allocator().template new_object<detail::sleep_resume_node>(std::move(prom));
  loop_ref.schedule_timer(*node, deadline);
  return std::move(fut);
}

// Returns a future<void> that becomes ready once `delay` elapses from
// now (est::platform::instance().now(), the same clock est::timer_queue
// itself is built on) - sugar over sleep_until() above.
[[nodiscard]] inline auto sleep_for(loop::clock::duration delay) -> future<void> {
  return sleep_until(platform::instance().now() + delay);
}

// Gives est::current_loop() the opportunity to run whatever else is
// already ready before the calling coroutine resumes - `co_await
// yield_execution();` inside a loop that would otherwise monopolize
// the ready-queue with back-to-back synchronous resumes (every
// `co_await` on an already-ready future skips suspension entirely,
// est:future's own `future_awaiter<T>::await_ready()`) lets other
// pending work interleave instead. Re-enters ready_ directly via
// detail::yield_resume_node rather than going through sleep_for(0): it
// has no real deadline to track, so there's no reason to pay for a
// timer_queue heap insert/pop, pending_timers_'s own linear search+erase
// on fire, or the platform::sleep_until() call run_impl() makes before
// it ever checks pending_timers_.
[[nodiscard]] inline auto yield_execution() -> future<void> {
  auto& loop_ref = current_loop();
  auto [prom, fut] = detail::make_promise_future_impl<void>(loop_ref.allocator());
  auto* node = loop_ref.allocator().template new_object<detail::yield_resume_node>(std::move(prom));
  loop_ref.enqueue_ready(*node);
  return std::move(fut);
}

} // namespace est
