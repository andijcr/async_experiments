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
    if (op.failed()) {
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
// still-pending sleep_resume_node (est:promise) out of the timer queue
// early cheap and safe). Not built on with_stop<T>() itself: with_stop()
// has no way to reach into the timer it's racing against to cancel it -
// this needs its own racer that also knows the timer_id.
//
// `done` is set before cancel_timer() is called (below), not after: if
// the timer had already fired naturally by the time the token wins the
// race, cancel_timer() returns false and is a no-op, but the timer's own
// completion (sleep_resume_node::fire(), est:promise, already run by then)
// would otherwise land on the same result via the first racer below -
// setting `done` first guarantees that racer sees it and backs off,
// however cancel_timer()'s own abandon()-driven cascade (if the timer
// was still pending) is scheduled to run.
struct sleep_stop_state {
  explicit sleep_stop_state(promise<void> result) noexcept : result(std::move(result)) {}

  promise<void> result;
  loop::timer_id id{};
  bool done = false;
};

} // namespace est::detail

export namespace est {

// Same as est::sleep_until() (est:promise), but the wait can be cut
// short: if `token` is stopped before `deadline`, the underlying timer is
// cancelled early (loop::cancel_timer(), est:loop - not merely
// stopped-watching, the pending sleep_resume_node is actually reclaimed)
// and the returned future fails with operation_cancelled instead of ever
// reaching `deadline`. See detail::sleep_stop_state's own doc comment
// above for the racer mechanics.
[[nodiscard]] inline auto sleep_until(loop::clock::time_point deadline, const stop_token& token)
    -> future<void> {
  if (token.stop_requested()) { // fast path - matches with_stop<T>()'s own,
    return make_failed_future<void>(operation_cancelled()); // never schedules a timer at all
  }

  auto& loop_ref = current_loop();
  auto [timer_prom, timer_fut] = detail::make_promise_future_impl<void>(current_allocator());
  auto* node = new detail::sleep_resume_node(std::move(timer_prom));
  const auto id = loop_ref.schedule_timer(*node, deadline);

  auto [prom, fut] = make_promise_future<void>();
  auto state = shared_ptr<detail::sleep_stop_state>::make(current_allocator(), std::move(prom));
  state->id = id;

  std::move(timer_fut).then_fast([state](future<void>& tf) {
    if (state->done) {
      return;
    }
    state->done = true;
    if (tf.failed()) {
      state->result.set_exception(tf.get_exception());
    } else {
      state->result.set_value();
    }
  });
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
