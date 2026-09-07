export module est:promise;

import std;
import :future;
import :loop;
import :platform;
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

} // namespace est

namespace est::detail {

// Wraps a nullary Fn (invoked once when the timer it's registered
// against fires) so est::loop::schedule_timer() has a concrete,
// allocator-destroyable detail::timer_node to hold - the same intrusive-
// virtual-node pattern est:future's own concrete_continuation<Fn, U>
// already uses (see that class's own doc comment), rather than a type-
// erased std::move_only_function that would bypass this codebase's own
// pmr-allocator plumbing.
template <class Fn> class concrete_timer_node final : public timer_node {
public:
  explicit concrete_timer_node(Fn fn) : fn_(std::move(fn)) {}

  void fire() override { fn_(); }

  // `this` here is concrete_timer_node<Fn>*, so delete_object deallocates
  // with this type's actual size/alignment - same reasoning as
  // concrete_continuation<Fn, U>::destroy() (est:future).
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept override {
    allocator.delete_object(this);
  }

private:
  Fn fn_;
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
// mutex::lock_resume_node's own doc comment gives (est:sync.mutex): a
// coroutine doing `co_await yield_execution(loop);` holds the resulting
// future<void> as a temporary spilled into its own frame across the
// suspension - the only other reference to that future_state<void>,
// besides this node. Silently dropping the promise on abandonment (this
// loop destroyed before ever draining this node) would leave that
// future_state permanently "not yet ready," which is also the only
// thing keeping the awaiting coroutine's own pending resume node
// reachable - neither side would ever free the other. Completing here
// breaks that, the same way it does for mutex's own resume nodes.
class yield_resume_node final : public ready_node {
public:
  explicit yield_resume_node(promise<void> prom) noexcept : promise_(std::move(prom)) {}

  void run() final {
    ran_ = true;
    promise_.set_value();
  }

  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept final {
    if (!ran_) {
      promise_.set_exception(std::make_exception_ptr(
          std::runtime_error("loop destroyed while yield_execution() was pending")));
    }
    allocator.delete_object(this);
  }

private:
  promise<void> promise_;
  bool ran_ = false;
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
  auto fire = [prom = std::move(prom)]() mutable { prom.set_value(); };
  using node_type = detail::concrete_timer_node<decltype(fire)>;
  auto* node = loop_ref.allocator().template new_object<node_type>(std::move(fire));
  loop_ref.schedule_timer(*node, deadline);
  return std::move(fut);
}

// Returns a future<void> that becomes ready once `delay` elapses from
// now (est::platform::instance().now(), the same clock est::timer_queue
// itself is built on, docs/PLAN.md M1) - sugar over sleep_until() above.
[[nodiscard]] inline auto sleep_for(loop& loop_ref, loop::clock::duration delay) -> future<void> {
  return sleep_until(loop_ref, platform::instance().now() + delay);
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

} // namespace est
