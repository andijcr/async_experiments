export module est:sync.stop_token;

import std;
import :future;
import :promise;
import :util.current_loop;

// Deliberately built directly on :future/:promise (make_promise_future_impl<void>()
// + promise<void>::get_future()/future<void>::clone()) rather than :sync.event's
// one_shot_event: :sync.event already depends on :promise (promise_resume_node<T>,
// its own wait() slow path), and the token-aware sleep_for()/sleep_until()
// overloads (est:promise) need :promise to depend on :sync.stop_token -
// importing :sync.event from here would close that into a cycle
// (:promise -> :sync.stop_token -> :sync.event -> :promise). Building on
// :future/:promise directly keeps this partition at the same DAG depth as
// :promise itself, not lower.
export namespace est {

// The exception a cancelled operation completes with - a second, distinct
// alternative to detail::abandoned_exception (est:loop), completed through
// the exact same future_state<T>::set_exception() channel abandonment
// already uses. Nothing in future_state<T>'s representation changes:
// cancellation is just a different exception flowing through the one
// "failed" alternative that already exists.
class operation_cancelled : public std::runtime_error {
public:
  operation_cancelled() : std::runtime_error("est: operation_cancelled") {}
};

class stop_source;

// A copyable, non-owning handle onto one stop_source's cancellation
// signal - any number of stop_token copies (independent holders, matching
// std::stop_token's own copyable-handle shape) may exist and outlive the
// stop_source that created them, since the shared future_state<void> stays
// alive regardless of which side drops first (the same est::shared_ptr
// est::future<T> is already built on, est:util.shared_ptr).
class stop_token {
public:
  // Non-blocking check - what with_stop()'s and the token-aware
  // sleep_for()/sleep_until()'s own already-cancelled fast paths use to
  // skip registering a continuation/scheduling a timer at all. Reads
  // future_state<void>'s own pending/value state via fut_.ready() directly
  // (synchronous the instant request_stop() calls set_value() - no
  // separate "fired" flag to keep in sync with it).
  [[nodiscard]] auto stop_requested() const noexcept -> bool { return fut_.ready(); }

  // A future<void> that becomes ready the moment request_stop() is
  // called (or already is, if it already was) - clone() (est:future)
  // lets any number of independent consumers co_await/then_fast() their
  // own copy of the same underlying signal.
  [[nodiscard]] auto stopped() const -> future<void> { return fut_.clone(); }

private:
  friend class stop_source;
  explicit stop_token(future<void> fut) noexcept : fut_(std::move(fut)) {}

  future<void> fut_;
};

// Owns the cancellation signal a stop_token (above) observes - request_stop()
// is the one thing that ever fires it. Mirrors est::mutex/est::counting_event's
// own allocator-first construction (current_allocator() default, :util.current_loop).
//
// Holds a bare promise<void>, not a promise/future pair behind a shared
// control block of its own: promise<T>/future<T> are already thin
// est::shared_ptr<future_state<T>> handles, so wrapping them in a second,
// separately-allocated struct (as an earlier version of this class did -
// detail::stop_state, now gone) only duplicated the sharing future_state<T>
// already provides. Every stop_token this class hands out derives its own
// future<void> on demand from prom_ via promise<void>::get_future()
// (est:promise) instead.
class stop_source {
public:
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

  explicit stop_source(allocator_type allocator = current_allocator())
      : prom_(make_promise(allocator)) {}

  // Idempotent: a second call is a harmless no-op, guarded by a freshly
  // derived future's own ready() rather than a separate bool - matching
  // the "independent cancellation sources safely racing to fire the same
  // signal" property one_shot_event::set() documents (est:sync.event),
  // reimplemented here directly since stop_source doesn't build on
  // one_shot_event (see this file's own top comment for why). A second
  // set_value() call would trip future_state<void>::check_not_completed()'s
  // assertion, so the guard is load-bearing, not just an optimization.
  void request_stop() noexcept {
    if (prom_.get_future().ready()) {
      return;
    }
    prom_.set_value();
  }

  [[nodiscard]] auto get_token() const noexcept -> stop_token {
    return stop_token(prom_.get_future());
  }

  [[nodiscard]] auto stop_requested() const noexcept -> bool { return prom_.get_future().ready(); }

private:
  // Unlike the removed detail::stop_state's own allocator parameter
  // (which only ever controlled where that now-gone wrapper struct
  // itself lived, never future_state<void>), `allocator` here reaches
  // detail::make_promise_future_impl() (est:promise) directly rather than
  // through the public make_promise_future<void>() (which always resolves
  // current_allocator() itself) - so it genuinely controls where this
  // stop_source's future_state<void> is allocated. The paired future half
  // make_promise_future_impl() also returns is discarded: nothing needs
  // it, since get_token()/stop_requested()/request_stop() all derive their
  // own future<void> from prom_ on demand instead of sharing one stored
  // here.
  static auto make_promise(allocator_type allocator) -> promise<void> {
    return detail::make_promise_future_impl<void>(allocator).first;
  }

  promise<void> prom_;
};

} // namespace est
