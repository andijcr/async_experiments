export module est:when_all;

import std;
import :future;
import :promise;
import :util.current_loop;
import :util.shared_ptr;

namespace est::detail {

// Shared state behind every est::when_all() call: counts down as each
// constituent future becomes ready (succeeded or failed - when_all()
// itself never inspects which), completing `result` once every one of
// them has. Lives exactly as long as the last constituent's own
// completion hook (below) does - `when_all()` itself drops its own
// reference the moment it returns, once every hook has been registered.
struct when_all_state {
  when_all_state(promise<void> result_in, int remaining_in)
      : result(std::move(result_in)), remaining(remaining_in) {}

  promise<void> result;
  int remaining;
};

// The per-future completion hook `when_all()` registers via `then()`.
// Explicitly typed on `future<T>&`, not a generic `auto&` parameter:
// then()'s own dispatch (future_state<T>::then()'s doc comment, est:future)
// checks unwrapped mode first, and a generic lambda would incidentally
// satisfy it too - callable with `const T&` via template argument
// deduction - which skips the callback entirely on failure
// (auto-propagate straight to the then()-returned future this code
// never even looks at). That's exactly wrong for when_all(): a failed
// constituent must count down `remaining` the same as a succeeded one.
// A `future<T>&` parameter can't bind a `const T&` argument, so
// invocable_unwrapped<Fn, T>() is false and wrapped mode - always
// invoked, success or failure - is guaranteed instead.
template <class T> auto when_all_hook(shared_ptr<when_all_state> state) {
  return [state = std::move(state)](future<T>& /*completed*/) {
    if (--state->remaining == 0) {
      state->result.set_value();
    }
  };
}

} // namespace est::detail

export namespace est {

// Resolves once every one of `futures` is ready, whether it succeeded or
// failed - when_all() never reads a value or an exception out of any of
// them itself. Each future<T>& stays owned by the caller (when_all()
// only ever registers a then() continuation on it, never consumes or
// moves it), so once the returned future<void> is ready, the caller
// inspects failed()/get() on whichever of `futures` it cares about,
// exactly as if it had awaited each one individually.
//
// An empty pack resolves immediately, ready() the moment when_all()
// returns - there is nothing left to wait for.
template <class... Ts> [[nodiscard]] auto when_all(future<Ts>&... futures) -> future<void> {
  auto [promise_, result] = make_promise_future<void>();
  if constexpr (sizeof...(Ts) == 0) {
    promise_.set_value();
  } else {
    auto state = shared_ptr<detail::when_all_state>::make(
        current_allocator(), std::move(promise_), static_cast<int>(sizeof...(Ts)));
    (futures.then(detail::when_all_hook<Ts>(state)), ...);
  }
  return std::move(result);
}

// Same, for a homogeneous, dynamically-sized run of futures instead of a
// fixed argument list - a std::span<future<T>> view over caller-owned
// storage (a std::vector<future<T>>, a std::array<future<T>, N>, ...),
// never taking ownership of the futures themselves. Template argument
// deduction can't see through a container's implicit conversion to
// std::span, so a caller passing anything other than an actual
// std::span<future<T>> needs to spell one out at the call site (e.g.
// `est::when_all(std::span(my_vector))`).
template <class T> [[nodiscard]] auto when_all(std::span<future<T>> futures) -> future<void> {
  auto [promise_, result] = make_promise_future<void>();
  if (futures.empty()) {
    promise_.set_value();
  } else {
    auto state = shared_ptr<detail::when_all_state>::make(
        current_allocator(), std::move(promise_), static_cast<int>(futures.size()));
    for (auto& f : futures) {
      f.then(detail::when_all_hook<T>(state));
    }
  }
  return std::move(result);
}

} // namespace est
