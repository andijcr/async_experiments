export module est:with_stop;

import std;
import :future;
import :loop;
import :platform;
import :promise;
import :sync.stop_token;
import :util.current_loop;
import :util.shared_ptr;

namespace est::detail {

// Shared between with_stop<T>()'s two racers (below) - `done` guards
// against both firing (operation completing naturally right as/after the
// token fires, or vice versa): exactly `when_any`'s own already-proven
// "shared_ptr<state> + two then_fast() racers, first one wins" shape
// (est:when_any), generalized to carry a real T through rather than just
// signalling completion.
template <class T> struct with_stop_state {
  explicit with_stop_state(promise<T> result) noexcept : result(std::move(result)) {}

  promise<T> result;
  bool done = false;
};

} // namespace est::detail

export namespace est {

// Races `operation` against `token.stopped()`: whichever completes first
// wins and is forwarded to the returned future<T>; the loser is a no-op,
// guarded by detail::with_stop_state<T>::done. If `operation` wins,
// its value/exception is forwarded unchanged; if the token wins, the
// returned future fails with operation_cancelled instead.
//
// Documented, honestly-scoped limitation - matching when_any()/when_all()/
// when_any_succeeds()'s own already-accepted one (est:when_any,
// est:when_all, est:when_any_succeeds): this stops the *caller* from
// waiting on `operation` any further, but does not eagerly free
// `operation` itself - it keeps running in the background until it
// completes on its own (that eventual completion just reaches an
// already-`done` guard and is silently dropped). Freeing a suspended
// coroutine/queued waiter eagerly would need intrusive_list<T>::remove()
// from the middle, which doesn't exist (est/src/util/intrusive_list.cppm -
// enqueue/dequeue/drain only) - out of scope here, same as it was for
// when_any()/when_all() before this issue. The one case where this
// codebase *does* cancel eagerly is a timed wait - see the token-aware
// sleep_for()/sleep_until() overloads below, built on loop::cancel_timer()
// instead of this general-purpose combinator.
template <class T>
[[nodiscard]] auto with_stop(future<T> operation, const stop_token& token) -> future<T> {
  if (token.stop_requested()) {                          // fast path - matches mutex::lock()'s own
    return make_failed_future<T>(operation_cancelled()); // uncontended path, when_all's
  } // empty-pack path, etc.

  auto [prom, fut] = make_promise_future<T>();
  auto state = shared_ptr<detail::with_stop_state<T>>::make(current_allocator(), std::move(prom));

  std::move(operation).then_fast([state](future<T>& op) {
    if (state->done) {
      return;
    }
    state->done = true;
    if (op.ready_with_failure()) {
      state->result.set_exception(op.get_exception());
    } else if constexpr (std::is_void_v<T>) {
      state->result.set_value();
    } else {
      state->result.set_value(std::move(op).get());
    }
  });
  token.stopped().then_fast([state](future<void>&) {
    if (state->done) {
      return;
    }
    state->done = true;
    state->result.set_exception(std::make_exception_ptr(operation_cancelled()));
  });
  return std::move(fut);
}

} // namespace est

namespace est::detail {

// Shared state behind the token-aware sleep_until()/sleep_for() overloads
// (below): races the underlying timer against `token.stopped()`, exactly
// like with_stop<T>() above does for an arbitrary future - except the
// token racer here also calls loop::cancel_timer(id), making this the one
// case in the codebase where cancellation is genuinely eager rather than
// just "stop watching" (loop::cancel_timer() makes pulling the
// still-pending timer node out of the timer queue early cheap and safe).
// Not built on with_stop<T>() itself: with_stop() has no way to reach
// into the timer it's racing against to cancel it - this needs its own
// racer that also knows the timer_id.
//
// `done` is set before cancel_timer() is called (below), not after: if
// the timer had already fired naturally by the time the token wins the
// race, cancel_timer() returns false and is a no-op, but the timer's own
// completion (sleep_stop_timer_node::fire(), just below, already run by
// then) would otherwise land on the same result via the first racer
// below - setting `done` first guarantees that racer sees it and backs
// off, however cancel_timer()'s own abandon()-driven cascade (if the
// timer was still pending) is scheduled to run.
struct sleep_stop_state {
  explicit sleep_stop_state(promise<void> result) noexcept : result(std::move(result)) {}

  promise<void> result;
  loop::timer_id id{};
  bool done = false;
};

// The deadline-timer racer itself: writes directly into the shared
// sleep_stop_state from fire()/abandon() instead of completing a second,
// bridging future_state<void> the way detail::sleep_resume_node
// (est:promise) would - that shape needs a then_fast() continuation on
// the bridge future just to unpack its result straight back out into this
// same sleep_stop_state, which is wasted work when the timer racer can
// write there directly: fewer allocations per call (one future_state<void>
// and one continuation node fewer), and, as a consequence, no
// intermediate ready_-queue node left for loop::drain_pending()'s second
// phase to abandon-instead-of-run (est:loop) - a still-pending
// sleep_stop_timer_node is abandoned directly, in drain_pending()'s first
// phase, which is what closes issue #113's gap for this combinator (est:
// with_timeout's own with_timeout_timer_node mirrors this same fix, for
// the identical reason).
//
// `done` is checked (and set) here too, not only in the two then_fast()
// racers below: loop::cancel_timer() (est:loop) calls abandon()
// synchronously, so the token racer winning and calling cancel_timer()
// reaches this same guard on the same call stack.
class sleep_stop_timer_node final : public timer_node,
                                    public current_allocator_new_delete<sleep_stop_timer_node> {
public:
  explicit sleep_stop_timer_node(shared_ptr<sleep_stop_state> state) noexcept
      : state_(std::move(state)) {}

  void fire() override {
    if (state_->done) {
      return;
    }
    state_->done = true;
    state_->result.set_value();
  }

  // abandoned_exception (est:loop) - reached when loop::drain_pending()
  // (est:loop) abandons this node still pending at teardown, and (as a
  // guarded no-op) when the token racer below wins and calls
  // loop::cancel_timer() on an already-`done` state - see this class's
  // own doc comment above.
  void abandon() noexcept override {
    if (state_->done) {
      return;
    }
    state_->done = true;
    state_->result.set_exception(std::make_exception_ptr(abandoned_exception()));
  }

  // operator new/delete inherited from current_allocator_new_delete<T>
  // (est:util.current_loop) - see sleep_resume_node's own doc comment
  // (est:promise) for why every concrete timer_node needs its own pair.

private:
  shared_ptr<sleep_stop_state> state_;
};

} // namespace est::detail

export namespace est {

// Same as est::sleep_until() (est:promise), but the wait can be cut
// short: if `token` is stopped before `deadline`, the underlying timer is
// cancelled early (loop::cancel_timer(), est:loop - not merely
// stopped-watching, the pending timer node is actually reclaimed) and the
// returned future fails with operation_cancelled instead of ever reaching
// `deadline`. See detail::sleep_stop_state/detail::sleep_stop_timer_node's
// own doc comments above for the racer mechanics - including why, unlike
// an earlier version of this function, loop teardown while both racers
// are still pending no longer leaves the returned future permanently
// pending (issue #113).
[[nodiscard]] inline auto sleep_until(loop::clock::time_point deadline, const stop_token& token)
    -> future<void> {
  if (token.stop_requested()) { // fast path - matches with_stop<T>()'s own,
    return make_failed_future<void>(operation_cancelled()); // never schedules a timer at all
  }

  auto& loop_ref = current_loop();
  auto [prom, fut] = make_promise_future<void>();
  auto state = shared_ptr<detail::sleep_stop_state>::make(current_allocator(), std::move(prom));

  // Guarded until schedule_timer() actually succeeds - see
  // est:with_timeout's own with_timeout<T>() for the identical hazard
  // this protects against (a bad_alloc there would otherwise leak the
  // node, since nothing else references it yet).
  std::unique_ptr<detail::sleep_stop_timer_node> timer_node_guard(
      new detail::sleep_stop_timer_node(state));
  state->id = loop_ref.schedule_timer(*timer_node_guard, deadline);
  timer_node_guard.release();

  token.stopped().then_fast([state](future<void>&) {
    if (state->done) {
      return;
    }
    state->done = true;
    [[maybe_unused]] const bool cancelled = current_loop().cancel_timer(state->id);
    state->result.set_exception(std::make_exception_ptr(operation_cancelled()));
  });
  return std::move(fut);
}

// Same as sleep_for() (est:promise), but token-cancellable - see the
// sleep_until(deadline, token) overload just above for the full doc
// comment; sugar over it exactly like est::sleep_for() is sugar over
// est::sleep_until().
[[nodiscard]] inline auto sleep_for(loop::clock::duration delay, const stop_token& token)
    -> future<void> {
  return sleep_until(platform::instance().now() + delay, token);
}

} // namespace est
