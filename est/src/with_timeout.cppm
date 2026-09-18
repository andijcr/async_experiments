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
// directly on :loop's schedule_timer()/cancel_timer() - the same two
// entry points est:with_stop's own sleep_until(deadline, const
// stop_token&) already builds on, for the identical reason.
namespace est::detail {

// Shared between with_timeout<T>()'s two racers (below): `id` is what the
// operation-racer needs to reach loop::cancel_timer() with (est:loop);
// `done` guards against both firing (operation completing naturally
// right as/after the deadline, or vice versa) - exactly with_stop<T>()'s
// own with_stop_state<T> shape (est:with_stop), generalized with the one
// extra field a timer race needs.
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

} // namespace est

namespace est::detail {

// The deadline-timer racer itself: writes directly into the shared
// with_timeout_state<T> from fire()/abandon() instead of completing a
// second, bridging future_state<void> the way detail::sleep_resume_node
// (est:promise) would - that shape (used by est:with_stop's own
// sleep_until(deadline, const stop_token&), which this class otherwise
// mirrors) needs a then_fast() continuation on the bridge future just to
// unpack its result straight back out into this same with_timeout_state<T>,
// which is wasted work when the timer racer can write there directly:
// fewer allocations per with_timeout<T>() call (one future_state<void> and
// one continuation node fewer), and, as a consequence, no intermediate
// ready_-queue node left for loop::drain_pending()'s second phase to
// abandon-instead-of-run (est:loop) - a still-pending with_timeout_timer_node
// is abandoned directly, in drain_pending()'s first phase, which is what
// makes this racer's own known limitation (below) strictly narrower than
// the bridged version's ever was.
//
// `done` is checked (and set) here rather than only in the operation
// racer's own then_fast() callback below: loop::cancel_timer() (est:loop)
// calls abandon() synchronously, so the operation racer winning and
// calling cancel_timer() reaches this same guard on the same call stack -
// without it, a timer that had already fired naturally (its own fire()
// already ran and set done) but whose entry is somehow still reachable
// would double-complete `state->result`, which future_state<T>::
// set_value()/set_exception()'s own check_not_completed() (est:future)
// would assert against.
template <class T>
class with_timeout_timer_node final
    : public timer_node,
      public current_allocator_new_delete<with_timeout_timer_node<T>> {
public:
  explicit with_timeout_timer_node(shared_ptr<with_timeout_state<T>> state) noexcept
      : state_(std::move(state)) {}

  void fire() override {
    if (state_->done) {
      return;
    }
    state_->done = true;
    state_->result.set_exception(std::make_exception_ptr(operation_timed_out()));
  }

  // abandoned_exception (est:loop) - reached when loop::drain_pending()
  // (est:loop) abandons this node still pending at teardown, propagating
  // that as-is rather than a fabricated timeout that never actually
  // happened. Also reached, as a guarded no-op, when the operation racer
  // below wins and calls loop::cancel_timer() on an already-`done` state -
  // see this class's own doc comment above for why the guard has to live
  // here rather than only in that racer.
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
  shared_ptr<with_timeout_state<T>> state_;
};

} // namespace est::detail

export namespace est {

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
//
// Unlike that limitation, loop teardown (loop::drain_pending(), est:loop -
// reached via make_current_loop()'s own guard or ~loop() alike) while both
// racers are still pending does NOT leave the returned future permanently
// pending: detail::with_timeout_timer_node's own abandon() (above)
// completes `state->result` directly, in drain_pending()'s first phase,
// with the same detail::abandoned_exception every other abandoned
// operation in this codebase fails with. (This was a real, documented gap
// in an earlier bridged-through-a-second-future shape this function used
// to have - issue #113 tracks the identical gap still open for est:
// with_stop's own sleep_until(deadline, const stop_token&), which hasn't
// been given the same fix yet.)
template <class T>
[[nodiscard]] auto with_timeout(future<T> operation, loop::clock::duration timeout) -> future<T> {
  auto& loop_ref = current_loop();

  auto [prom, fut] = make_promise_future<T>();
  auto state =
      shared_ptr<detail::with_timeout_state<T>>::make(current_allocator(), std::move(prom));

  // Guarded until schedule_timer() actually succeeds: it does a real
  // allocation of its own (pending_timers_.reserve(), timer_queue::
  // schedule_at(), neither noexcept, est:loop) - without this, a bad_alloc
  // there would leak the node, since nothing else references it yet.
  // Released (ownership transferred to loop's own pending_timers_) only on
  // the line right after a successful call.
  auto timer_node_guard = std::make_unique<detail::with_timeout_timer_node<T>>(state);
  state->id = loop_ref.schedule_timer(*timer_node_guard, platform::instance().now() + timeout);
  timer_node_guard.release();

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
  return std::move(fut);
}

} // namespace est
