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
//
// Fn constrained the same way est::scope_exit constrains its own stored
// Fn (est:util.scope_exit) - at the type, not just at schedule_periodic()
// below - so a bad Fn fails with a constraint diagnostic pointing at the
// actual mismatch, not a template-instantiation error buried inside
// fire()'s own body. std::invocable<Fn&>, not plain std::invocable<Fn>:
// fn_ is invoked repeatedly, as a named (non-const lvalue) member, once
// per period - matching future.cppm's own Fn&-based invocable checks for
// a callable stored and invoked more than once, rather than scope_exit's
// own plain Fn (invoked exactly once, from a destructor).
template <class Fn>
  requires std::invocable<Fn&>
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
    // Measured *before* fn_() runs, not after: the next deadline is
    // anchored to when this period started, not to whenever fn_()
    // happened to finish - fixed-rate scheduling (like setInterval()),
    // not fixed-delay. Without this, a slow or variable-latency fn_()
    // would make the chain's real cadence drift away from `interval` by
    // however long each call took, compounding period over period.
    const auto period_start = platform::instance().now();
    // __cpp_exceptions gate: same reasoning as future.cppm's own
    // concrete_continuation<Fn, U>::run() comment - fn_ genuinely can
    // throw when exceptions are enabled, but -fno-exceptions makes
    // `throw` illegal everywhere in the TU (fn_'s own body included), so
    // this catch is unreachable dead code the compiler won't let this
    // function spell on such a build.
#ifdef __cpp_exceptions
    try {
      fn_();
    } catch (...) {
      // fn_ has no downstream future to route an exception into (unlike
      // future_state<T>'s own continuations, concrete_continuation<Fn, U>::
      // run(), est:future) - fn_ returns void, not a future<T>. Letting it
      // escape here would unwind loop::fire_ready_timers()/run_impl()
      // entirely, abandoning every other unrelated pending timer and
      // ready-work item on the same loop over one periodic callback's own
      // bug - and silently kill this chain forever with no diagnostic.
      // Reported the same way loop::run_one()'s own long-running-callback
      // stall is (platform::printdbg() - a loud diagnostic that doesn't
      // stop the loop), then treated as one skipped period: the chain
      // still reschedules below, so a transient failure doesn't
      // permanently kill an otherwise-healthy periodic job.
      platform::printdbg(
          "est::schedule_periodic: fn() threw an exception - period skipped, chain continues");
    }
#else
    fn_();
#endif
    if (ctrl_->cancelled) { // fn_ itself may have just cancelled
      return;
    }
    auto& loop_ref = current_loop(); // resolved fresh - see est::counting_event's
                                     // own doc comment (est:sync.event) for the
                                     // identical cross-loop hazard every
                                     // loop-resolving call in this codebase
                                     // already documents
    const auto offset = jitter_();
    // Guarded until schedule_timer() actually succeeds (it does a real
    // allocation of its own, not noexcept, est:loop) - without this, a
    // bad_alloc there would leak `next`, since nothing else references it
    // yet. Released only on the line right after a successful call.
    auto next = std::make_unique<periodic_timer_node>(std::move(fn_), interval_, jitter_, ctrl_);
    loop_ref.schedule_timer(*next, period_start + interval_ + offset);
    next.release();
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
// phase relative to each other. Fixed-rate, not fixed-delay: each
// period's deadline is measured from when the *previous* one started,
// not from when fn() returned, so a slow or variable-latency fn()
// doesn't drift the chain's average cadence away from `interval` - see
// detail::periodic_timer_node<Fn>::fire()'s own doc comment. `interval`
// must be positive and `max_jitter` must be strictly less than `interval`
// (both checked): jitter perturbs a period, it never gets to invert or
// collapse one - a jittered delay of exactly zero would risk the
// rescheduled node landing in the very timer batch that's still firing
// (est::loop::fire_ready_timers(), est:loop, evaluates "now" once per
// batch), refiring before returning to est::loop's own outer loop.
//
// Against est::current_loop() - no explicit loop& overload, matching
// every other timer-driven entry point in this codebase (sleep_for(),
// yield_execution()).
//
// Fn must be std::invocable<Fn&> - see detail::periodic_timer_node<Fn>'s
// own doc comment for why Fn&, not plain Fn.
template <class Fn>
  requires std::invocable<Fn&>
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
  // Guarded until schedule_timer() actually succeeds - see
  // detail::periodic_timer_node<Fn>::fire()'s own re-arming call, just
  // above, for the identical hazard this protects against.
  auto node = std::make_unique<detail::periodic_timer_node<Fn>>(std::move(fn), interval, jit, ctrl);
  loop_ref.schedule_timer(*node, platform::instance().now() + interval + first_offset);
  node.release();
  return periodic_timer_handle(std::move(ctrl));
}

} // namespace est
