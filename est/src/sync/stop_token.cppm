export module est:sync.stop_token;

import std;
import :future;
import :promise;
import :util.current_loop;
import :util.shared_ptr;

// Deliberately built directly on :future/:promise (make_promise_future<void>()
// + future<void>::clone()) rather than :sync.event's one_shot_event:
// :sync.event already depends on :promise (promise_resume_node<T>, its own
// wait() slow path), and the token-aware sleep_for()/sleep_until()
// overloads (est:promise) need :promise to depend on :sync.stop_token -
// importing :sync.event from here would close that into a cycle
// (:promise -> :sync.stop_token -> :sync.event -> :promise). Building on
// make_promise_future<void>() directly keeps this partition at the same
// DAG depth as :promise itself, not lower.
namespace est::detail {

struct stop_state {
  promise<void> prom;
  future<void> fut;
};

} // namespace est::detail

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
// stop_source that created them, since the shared est::shared_ptr keeps
// detail::stop_state alive regardless of which side drops first.
class stop_token {
public:
  // Non-blocking check - what with_stop()'s and the token-aware
  // sleep_for()/sleep_until()'s own already-cancelled fast paths use to
  // skip registering a continuation/scheduling a timer at all. Reads
  // future_state<void>'s own pending/value state via fut.ready() directly
  // (synchronous the instant request_stop() calls set_value() - no
  // separate "fired" flag to keep in sync with it).
  [[nodiscard]] auto stop_requested() const noexcept -> bool { return state_->fut.ready(); }

  // A future<void> that becomes ready the moment request_stop() is
  // called (or already is, if it already was) - clone() (est:future)
  // lets any number of independent consumers co_await/then_fast() their
  // own copy of the same underlying signal.
  [[nodiscard]] auto stopped() const -> future<void> { return state_->fut.clone(); }

private:
  friend class stop_source;
  explicit stop_token(shared_ptr<detail::stop_state> state) noexcept : state_(std::move(state)) {}

  shared_ptr<detail::stop_state> state_;
};

// Owns the cancellation signal a stop_token (above) observes - request_stop()
// is the one thing that ever fires it. Mirrors est::mutex/est::counting_event's
// own allocator-first construction (current_allocator() default, :util.current_loop).
class stop_source {
public:
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

  explicit stop_source(allocator_type allocator = current_allocator())
      : state_(make_state(allocator)) {}

  // Idempotent: a second call is a harmless no-op, guarded by fut.ready()
  // rather than a separate bool - matching the "independent cancellation
  // sources safely racing to fire the same signal" property
  // one_shot_event::set() documents (est:sync.event), reimplemented here
  // directly since stop_state doesn't build on one_shot_event (see this
  // file's own top comment for why). A second set_value() call would trip
  // future_state<void>::check_not_completed()'s assertion, so the guard
  // is load-bearing, not just an optimization.
  void request_stop() noexcept {
    if (state_->fut.ready()) {
      return;
    }
    state_->prom.set_value();
  }

  [[nodiscard]] auto get_token() const noexcept -> stop_token { return stop_token(state_); }

  [[nodiscard]] auto stop_requested() const noexcept -> bool { return state_->fut.ready(); }

private:
  // make_promise_future<void>() always resolves current_allocator()
  // internally for the future_state<void> it builds (see its own doc
  // comment, est:promise) - `allocator` here only ever controls where
  // this stop_source's own detail::stop_state control block lives. The
  // two coincide whenever a caller relies on the default parameter
  // (current_allocator() itself), the same way loop's own constructor
  // and every other allocator-first type in this codebase does; passing
  // an explicit, different allocator here only changes where stop_state
  // itself is allocated, not where its nested promise<void>/future<void>
  // pair's future_state<void> is.
  static auto make_state(allocator_type allocator) -> shared_ptr<detail::stop_state> {
    auto [prom, fut] = make_promise_future<void>();
    return shared_ptr<detail::stop_state>::make(allocator, std::move(prom), std::move(fut));
  }

  shared_ptr<detail::stop_state> state_;
};

} // namespace est
