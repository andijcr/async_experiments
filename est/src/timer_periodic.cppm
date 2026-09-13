export module est:timer.periodic;

import std;
import :check;
import :loop;
import :platform;
import :util.current_loop;
import :util.jitter;
import :util.shared_ptr;

namespace est::detail {

// Shared across every period of one schedule_periodic() call:
// periodic_timer_handle::cancel() (below) flips `cancelled` to stop the
// chain. A plain bool, not atomic - this cancellation is entirely
// loop-thread-side, the same single-threaded assumption as everywhere
// else in est (contrast a hypothetical cross-thread signal, which would
// need real synchronization - nothing here does).
struct periodic_timer_control {
  bool cancelled = false;
};

// The node behind schedule_periodic() (below): re-arms itself by handing
// off to a *fresh* instance via loop::schedule_timer() each period,
// rather than reusing itself in place. loop::fire_ready_timers()
// (est:loop) unconditionally deletes every timer_node right after fire()
// returns - the same one-shot contract every other timer_node in this
// codebase relies on - so a self-rescheduling timer has to hand off to a
// successor instead of surviving its own firing.
template <class Fn>
class periodic_timer_node final : public timer_node,
                                  public current_allocator_new_delete<periodic_timer_node<Fn>> {
public:
  periodic_timer_node(Fn fn,
                      loop::clock::duration interval,
                      jitter jit,
                      shared_ptr<periodic_timer_control> ctrl)
      // jit taken by value, not moved into jitter_: est::jitter is
      // trivially copyable (two small integers' worth of state), so a
      // move buys nothing over a copy here - clang-tidy's own
      // performance-move-const-arg agrees.
      : fn_(std::move(fn)), interval_(interval), jitter_(jit), ctrl_(std::move(ctrl)) {}

  void fire() override {
    if (ctrl_->cancelled) {
      return;
    }
    fn_();
    if (ctrl_->cancelled) { // fn_ itself may have just cancelled
      return;
    }
    auto& loop_ref = current_loop(); // resolved fresh - see est::counting_event's
                                     // own doc comment (est:sync.event) for the
                                     // identical cross-loop hazard every
                                     // loop-resolving call in this codebase
                                     // already documents
    const auto offset = jitter_();
    auto* next = new periodic_timer_node(std::move(fn_), interval_, jitter_, ctrl_);
    loop_ref.schedule_timer(*next, platform::instance().now() + interval_ + offset);
  }

  // No abandon() override: a periodic chain dying alongside its loop (or
  // cancelled) needs no completion of its own - nothing ever awaits a
  // periodic timer the way a coroutine awaits sleep_for().

private:
  Fn fn_;
  loop::clock::duration interval_;
  jitter jitter_;
  shared_ptr<periodic_timer_control> ctrl_;
};

} // namespace est::detail

export namespace est {

// A live schedule_periodic() registration - lets a caller stop further
// re-arming. Destroying the handle does *not* cancel (matching every
// other handle-shaped type in this codebase - a promise<T>, a future<T> -
// being silently dropped rather than triggering an implicit action): call
// cancel() explicitly.
class periodic_timer_handle {
public:
  explicit periodic_timer_handle(shared_ptr<detail::periodic_timer_control> ctrl) noexcept
      : ctrl_(std::move(ctrl)) {}

  void cancel() noexcept { ctrl_->cancelled = true; }

private:
  shared_ptr<detail::periodic_timer_control> ctrl_;
};

// Calls fn() every `interval`, plus a fresh, uniformly distributed offset
// in [-max_jitter, +max_jitter] each period (est::jitter, :util.jitter) -
// spreading out otherwise-lockstep wakeups (several independent periodic
// sources sharing one loop) instead of always firing at exactly the same
// phase relative to each other. `interval` must be positive and
// `max_jitter` must be strictly less than `interval` (both checked):
// jitter perturbs a period, it never gets to invert or collapse one - a
// jittered delay of exactly zero would risk the rescheduled node landing
// in the very timer batch that's still firing (est::loop::fire_ready_timers(),
// est:loop, evaluates "now" once per batch), refiring before returning to
// est::loop's own outer loop.
//
// Against est::current_loop() - no explicit loop& overload, matching
// every other timer-driven entry point in this codebase (sleep_for(),
// yield_execution()).
template <class Fn>
[[nodiscard]] auto
schedule_periodic(loop::clock::duration interval, Fn fn, loop::clock::duration max_jitter = {})
    -> periodic_timer_handle {
  check(interval > loop::clock::duration::zero(),
        "est::schedule_periodic: interval must be positive");
  check(max_jitter < interval, "est::schedule_periodic: max_jitter must be less than interval");
  auto& loop_ref = current_loop();
  auto ctrl = shared_ptr<detail::periodic_timer_control>::make(current_allocator());
  jitter jit(max_jitter);
  const auto first_offset = jit();
  auto* node = new detail::periodic_timer_node<Fn>(std::move(fn), interval, jit, ctrl);
  loop_ref.schedule_timer(*node, platform::instance().now() + interval + first_offset);
  return periodic_timer_handle(std::move(ctrl));
}

} // namespace est
