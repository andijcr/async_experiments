export module est:when_all;

import std;
import :future;
import :promise;
import :util.current_loop;
import :util.shared_ptr;

namespace est::detail {

// Shared state behind every est::when_all() call: counts down as each
// constituent future is accounted for - completed or abandoned,
// succeeded or failed (when_all() itself never inspects which) -
// completing `result` once every one of them has. Lives exactly as long
// as the last constituent's own tracking chain (when_all_track(), below)
// does - `when_all()` itself drops its own reference the moment it
// returns, once every input has been registered.
struct when_all_state {
  when_all_state(promise<void> result_in, int remaining_in)
      : result(std::move(result_in)), remaining(remaining_in) {}

  promise<void> result;
  int remaining;
};

// Registers `state`'s completion accounting against `input` - two
// `.then()` stages, not one, and each explicitly typed on a concrete
// future<T>&/future<void>& parameter rather than a generic `auto&` one.
// Either shortcut breaks correctness:
//
// - A generic `[](auto& completed) {...}` hook would incidentally
//   satisfy then()'s unwrapped-mode dispatch too (a template parameter
//   binds to `const T&` just as readily as to `future<T>&`), which
//   auto-propagates a failure to the (here, discarded) then()-returned
//   future *without ever calling the hook at all* - a failed input would
//   never decrement `remaining`.
// - A hook registered directly on `input` - even one correctly typed on
//   future<T>& to force wrapped mode - is *still* never invoked if
//   `input`'s own future_state is abandoned (destroyed while still
//   pending - e.g. a helper function starts some async producer, calls
//   when_all() on the future it hands back, and returns, letting its own
//   local promise/future pair go out of scope once nothing local needs
//   them any more) rather than actually completed:
//   concrete_continuation<Fn, U>::abandon() (est:future) unconditionally
//   completes *its own* downstream with an exception, without ever
//   invoking `fn_` - so a counting hook living there would simply never
//   run, hanging when_all()'s returned future forever the moment any one
//   input was abandoned instead of completed.
//
// The fix is this two-stage chain: a no-op first stage whose only job is
// to produce a future<void> that reliably completes whenever `input`
// does, *whichever way that happens*. abandon()'s own exception-
// completion goes through future_state<T>::complete() exactly the same
// way a normal set_value()/set_exception() call does, waking up
// second-stage waiters identically either way - so the real counting
// logic, living in that second stage, always runs exactly once per
// input, abandoned or not. See docs/wiki/Continuation-Node-Mechanism.md's
// "est::when_all(): forcing wrapped mode on purpose" for the full account.
template <class T> void when_all_track(future<T>& input, shared_ptr<when_all_state> state) {
  input.then([](future<T>&) {}).then([state = std::move(state)](future<void>&) {
    if (--state->remaining == 0) {
      state->result.set_value();
    }
  });
}

// Builds the promise<void>/future<void> pair when_all() itself returns,
// plus the when_all_state its constituent hooks share - shared by both
// overloads below, which differ only in how they arrive at `count` and
// how they iterate their inputs. `count == 0` resolves the returned
// future immediately and skips allocating a when_all_state entirely -
// there's nothing left to track, and the returned (default-constructed,
// empty) shared_ptr is never dereferenced: neither overload's fold
// expression/loop has anything to iterate when count is 0.
[[nodiscard]] inline auto when_all_setup(int count)
    -> std::pair<shared_ptr<when_all_state>, future<void>> {
  auto [promise_, result] = make_promise_future<void>();
  if (count == 0) {
    promise_.set_value();
    return {shared_ptr<when_all_state>{}, std::move(result)};
  }
  auto state = shared_ptr<when_all_state>::make(current_allocator(), std::move(promise_), count);
  return {std::move(state), std::move(result)};
}

} // namespace est::detail

export namespace est {

// Resolves once every one of `futures` is accounted for - completed or
// abandoned, succeeded or failed - when_all() never reads a value or an
// exception out of any of them itself. Each future<T>& stays owned by
// the caller (when_all() only ever registers then() continuations on
// it, never consumes or moves it), so once the returned future<void> is
// ready, the caller inspects failed()/get() on whichever of `futures` it
// cares about, exactly as if it had awaited each one individually.
//
// An empty pack resolves immediately, ready() the moment when_all()
// returns - there is nothing left to wait for.
template <class... Ts> [[nodiscard]] auto when_all(future<Ts>&... futures) -> future<void> {
  auto [state, result] = detail::when_all_setup(static_cast<int>(sizeof...(Ts)));
  (detail::when_all_track<Ts>(futures, state), ...);
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
  auto [state, result] = detail::when_all_setup(static_cast<int>(futures.size()));
  for (auto& f : futures) {
    detail::when_all_track<T>(f, state);
  }
  return std::move(result);
}

} // namespace est
