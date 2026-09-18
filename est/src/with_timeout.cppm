export module est:with_timeout;

import std;
import :future;
import :loop;
import :platform;
import :promise;
import :util.current_loop;
import :util.shared_ptr;

// Not built on est::with_stop<T>()/est::stop_token (est:with_stop,
// est:sync.stop_token), despite issue #57's own original proposed shape
// ("essentially when_any(operation, sleep_for(timeout))"): neither
// primitive has a way to reach into the timer it's racing against to
// cancel it early, so composing from them would leave a fired-but-unused
// deadline timer sitting in loop's timer queue for its full duration
// even after `operation` already won. Built instead as its own racer,
// directly on :promise's own building blocks, exactly the shape
// est:with_stop's own sleep_until(deadline, const stop_token&) already
// established for the identical reason (that overload isn't built on
// with_stop<T>() either - see its own doc comment) - loop::cancel_timer()
// needs the timer_id schedule_timer() returns, which only the code that
// scheduled the timer ever has.
namespace est::detail {

// Mirrors est:with_stop's own sleep_stop_state exactly, with which side
// wins inverted: there, the stop_token winning cancels the timer; here,
// `operation` winning does - the deadline timer is the thing being raced
// against, not the thing racing on behalf of a caller-controlled signal.
template <class T> struct with_timeout_state {
  explicit with_timeout_state(promise<T> result) noexcept : result(std::move(result)) {}

  promise<T> result;
  loop::timer_id id{};
  bool done = false;
};

} // namespace est::detail

export namespace est {

// The exception a timed-out with_timeout<T>() call fails with - a third,
// distinct alternative alongside detail::abandoned_exception (est:loop)
// and operation_cancelled (est:sync.stop_token), completed through the
// same future_state<T>::set_exception() channel both already use. Kept
// separate from operation_cancelled rather than reused: with_timeout<T>()
// has no stop_token/stop_source of its own (it doesn't use either), so a
// caller catching operation_cancelled elsewhere to mean "something
// explicitly requested cancellation" shouldn't also have to catch it for
// "this simply took too long" - a different condition, worth its own type.
class operation_timed_out : public std::runtime_error {
public:
  operation_timed_out() : std::runtime_error("est: operation_timed_out") {}
};

// Races `operation` against a deadline timer (`timeout` from now,
// est::platform::instance().now() - the same clock est::timer_queue
// itself is built on, matching sleep_for()'s own est:promise sugar over
// sleep_until()): whichever completes first wins and is forwarded to the
// returned future<T>. If `operation` wins, its value/exception is
// forwarded unchanged, and the now-redundant deadline timer is reclaimed
// immediately via loop::cancel_timer() rather than left to fire uselessly
// later (est:loop) - genuinely eager, not just "stop watching," the same
// property sleep_until(deadline, const stop_token&) already has (est:
// with_stop). If the timer wins, the returned future fails with
// operation_timed_out instead.
//
// Same honestly-scoped limitation as with_stop<T>()/when_any()/when_all()/
// when_any_succeeds() (est:with_stop, est:when_any, est:when_all,
// est:when_any_succeeds): a lost race only stops the *caller* from
// waiting on `operation` further - it keeps running in the background
// until it completes on its own (that eventual completion reaches an
// already-`done` guard and is silently dropped). Eagerly freeing an
// arbitrary suspended coroutine or queued waiter would need
// intrusive_list<T>::remove() from the middle, which doesn't exist - see
// those combinators' own doc comments for the same gap.
template <class T>
[[nodiscard]] auto with_timeout(future<T> operation, loop::clock::duration timeout) -> future<T> {
  auto& loop_ref = current_loop();
  auto [timer_prom, timer_fut] = detail::make_promise_future_impl<void>(current_allocator());
  auto* timer_node = new detail::sleep_resume_node(std::move(timer_prom));
  const auto id = loop_ref.schedule_timer(*timer_node, platform::instance().now() + timeout);

  auto [prom, fut] = make_promise_future<T>();
  auto state =
      shared_ptr<detail::with_timeout_state<T>>::make(current_allocator(), std::move(prom));
  state->id = id;

  std::move(operation).then_fast([state](future<T>& op) {
    if (state->done) {
      return;
    }
    state->done = true;
    [[maybe_unused]] const bool cancelled = current_loop().cancel_timer(state->id);
    if (op.ready_with_failure()) {
      state->result.set_exception(op.get_exception());
    } else if constexpr (std::is_void_v<T>) {
      state->result.set_value();
    } else {
      state->result.set_value(std::move(op).get());
    }
  });
  std::move(timer_fut).then_fast([state](future<void>& tf) {
    if (state->done) {
      return;
    }
    state->done = true;
    if (tf.ready_with_failure()) {
      // Abandoned (loop torn down while both were still pending) -
      // propagate that as-is, not a fabricated timeout that didn't
      // actually happen. Not currently reachable through
      // loop::drain_pending() (est:loop) as of this writing: its own
      // single pass abandons pending_timers_ first, which can complete
      // this exact node's own waiters_ and re-enqueue it into ready_,
      // but drain_pending() then abandons whatever's freshly sitting in
      // ready_ too, in that same call - so this then_fast() callback
      // never actually runs via that path, same as sleep_until(deadline,
      // const stop_token&)'s own analogous branch (est:with_stop). Kept
      // for the same reason that one is: documents the intended
      // behavior and survives if drain_pending()'s single-pass shape
      // ever changes (e.g. under #103's registry rearchitecture).
      state->result.set_exception(tf.get_exception());
    } else {
      state->result.set_exception(std::make_exception_ptr(operation_timed_out()));
    }
  });
  return std::move(fut);
}

} // namespace est
