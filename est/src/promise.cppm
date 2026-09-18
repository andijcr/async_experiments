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

  // Returns a future<T> aliasing the same future_state as *this - the
  // producer-side mirror of future<T>::clone() (est:future): any number
  // of independent future<T> handles can be derived straight from a
  // promise, each seeing the same eventual result, without the caller
  // needing to have already held on to one. Unlike a hypothetical
  // future<T>::get_promise() (considered and rejected - see docs/PLAN.md),
  // this doesn't fabricate a second producer: promise<T> stays exactly as
  // move-only as ever, so at most one entity can ever call
  // set_value()/set_exception() on a given future_state<T> - get_future()
  // only ever adds more consumers, the same safe direction clone() already
  // supports. Constrained to T = void or a scalar T for the identical
  // reason clone() is - see its own doc comment for the moved-from hazard
  // this sidesteps.
  [[nodiscard]] auto get_future() const -> future<T>
    requires(std::is_void_v<T> || std::is_scalar_v<T>)
  {
    return future<T>(state_);
  }

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
  return detail::make_promise_future_impl<T>(current_allocator());
}

// Builds an already-ready future<T> against est::current_loop(),
// constructing its value in place from `args...` - sugar over
// make_promise_future<T>() followed by promise<T>::set_value(T(args...)),
// for a caller that doesn't need to hold the promise itself.
template <class T, class... Args>
[[nodiscard]] auto make_ready_future(Args&&... args) -> future<T> {
  static_assert(!std::is_void_v<T> || sizeof...(Args) == 0,
                "make_ready_future<void>() takes no arguments");
  auto [prom, fut] = make_promise_future<T>();
  if constexpr (std::is_void_v<T>) {
    prom.set_value();
  } else {
    prom.set_value(T(std::forward<Args>(args)...));
  }
  return std::move(fut);
}

// The failure-case sibling to make_ready_future() above: builds a fresh
// future<T> against est::current_loop(), already completed with
// `exception` - sugar over make_promise_future<T>() followed by
// promise<T>::set_exception(), for a caller that doesn't need to hold the
// promise itself (e.g. an already-cancelled fast path that never needs to
// schedule anything).
// By-value on purpose: `exception` is std::move()-d into
// std::make_exception_ptr() below, but clang-tidy's dataflow can't see
// through that dependent (template-parameter-typed) call to confirm it,
// and flags the parameter as copied-but-only-read regardless.
template <class T, class Exception>
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_failed_future(Exception exception) -> future<T> {
  auto [prom, fut] = make_promise_future<T>();
  prom.set_exception(std::make_exception_ptr(std::move(exception)));
  return std::move(fut);
}

} // namespace est

namespace est::detail {

// The node behind sleep_for()/sleep_until() (below): holds a
// promise<void> directly rather than a generic Fn, so abandon() can
// complete that promise on abandonment - a type-erased Fn would give
// abandon() no way to know it's holding a promise at all, let alone call
// set_exception() on it. Without this, a coroutine doing `co_await
// sleep_for(10s);`, abandoned when its loop is destroyed before the
// timer ever fires, would leak its own frame forever - the same
// abandon()-driven exception-completion detail::promise_resume_node<T>
// below also relies on, for the identical reason: the awaiting coroutine
// holds the only other reference to this promise's future_state<void>
// (the future<void> temporary co_await awaits is spilled into the
// coroutine's own frame across the suspension), so silently dropping the
// promise instead of completing it would strand that frame with nothing
// left to free it.
//
// Not folded into promise_resume_node<T> below (issue #77's own
// suggestion): the two differ in more than just which base they need -
// sleep_resume_node needs a timer_node base (fire(), scheduled via
// schedule_timer()) where promise_resume_node<T> needs a ready_node one
// (run(), scheduled via enqueue_ready()), and a shared base can't
// straddle both without either type losing its own single most useful
// property (being exactly the node type its own loop container expects).
class sleep_resume_node final : public timer_node,
                                public current_allocator_new_delete<sleep_resume_node> {
public:
  explicit sleep_resume_node(promise<void> prom) noexcept : promise_(std::move(prom)) {}

  void fire() override { promise_.set_value(); }

  // abandoned_exception (est:loop) - shared with every other abandon()
  // override in this codebase that needs to actually complete something,
  // rather than one hand-rolled literal per call site.
  void abandon() noexcept override {
    promise_.set_exception(std::make_exception_ptr(abandoned_exception()));
  }

  // operator new/delete inherited from current_allocator_new_delete<T>
  // (est:util.current_loop) - see that class's own doc comment for why
  // every concrete ready_node/timer_node needs its own pair rather than
  // one shared at the ready_node/timer_node base itself.

private:
  promise<void> promise_;
};

// The node behind yield_execution() (below) and counting_event<Mode>::
// wait()'s slow path (est:sync.event) alike: a plain ready_node holding
// a promise<T>, completed with promise_.set_value() on a successful
// run() or, on abandonment, with abandoned_exception (est:loop). Both
// call sites used to hand-roll their own, byte-identical copy of this
// exact node (yield_resume_node here, event_resume_node in
// est:sync.event) - collapsed into this one shared type per issue #77.
// (sleep_resume_node, above, stays separate - see its own doc comment
// for why.) Templated on T purely for the same reason est:future's
// future_resume_node<T>/concrete_continuation<Fn, U> are - both current
// instantiations are promise_resume_node<void> (run() calling
// promise_.set_value() with no argument only compiles for T = void, so
// nothing else could compile against this class today regardless), but
// there's nothing else in run()/abandon() that's specific to void
// either.
//
// Not nested inside est::mutex or est::counting_event<Mode> the way an
// earlier version of one of these two (mutex::lock_resume_node, now
// gone - est::mutex is built directly on est::counting_event since issue
// #67) once was: run()/abandon() never touch anything about whichever
// type enqueued this node, so a single free class in est::detail serves
// every caller instead of minting an identical type per caller.
template <class T>
class promise_resume_node final : public ready_node,
                                  public current_allocator_new_delete<promise_resume_node<T>> {
public:
  explicit promise_resume_node(promise<T> prom) noexcept : promise_(std::move(prom)) {}

  void run() final { promise_.set_value(); }

  // abandoned_exception (est:loop) - shared with every other abandon()
  // override in this codebase that needs to actually complete something,
  // rather than one hand-rolled literal per call site.
  void abandon() noexcept final {
    promise_.set_exception(std::make_exception_ptr(abandoned_exception()));
  }

  // operator new/delete inherited from current_allocator_new_delete<T>
  // (est:util.current_loop) - see sleep_resume_node's own doc comment
  // (above) for why.

private:
  promise<T> promise_;
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
  auto [prom, fut] = detail::make_promise_future_impl<void>(current_allocator());
  // Guarded until schedule_timer() actually succeeds: it does a real
  // allocation of its own (pending_timers_.reserve(), timer_queue::
  // schedule_at(), neither noexcept, est:loop) - without this, a bad_alloc
  // there would leak the node, since nothing else references it yet.
  // Released (ownership transferred to loop's own pending_timers_) only on
  // the line right after a successful call - same idiom est:with_timeout/
  // est:with_stop's own timer-racer nodes use.
  auto node = std::make_unique<detail::sleep_resume_node>(std::move(prom));
  loop_ref.schedule_timer(*node, deadline);
  node.release();
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
// detail::promise_resume_node<void> rather than going through sleep_for(0): it
// has no real deadline to track, so there's no reason to pay for a
// timer_queue heap insert/pop, pending_timers_'s own linear search+erase
// on fire, or the platform::sleep_until() call run_impl() makes before
// it ever checks pending_timers_.
[[nodiscard]] inline auto yield_execution() -> future<void> {
  auto& loop_ref = current_loop();
  auto [prom, fut] = detail::make_promise_future_impl<void>(current_allocator());
  auto node = std::make_unique<detail::promise_resume_node<void>>(std::move(prom));
  // Stamped from current_priority(), not left at ready_node's own
  // Priority::normal default: co_await yield_execution() re-enters
  // ready_ directly rather than through future_awaiter<T>::
  // await_suspend() (est:future), which is the usual place a co_await
  // inherits the calling coroutine's ambient priority - without this,
  // a coroutine running at Priority::critical would drop to normal the
  // instant it yields, exactly the priority inversion issue #31's whole
  // inheritance mechanism exists to prevent.
  node->priority_level = current_priority();
  loop_ref.enqueue_ready(*node); // noexcept (est:loop) - release() right after is still the
  node.release();                // same ownership-transfer idiom every site here uses
  return std::move(fut);
}

} // namespace est
