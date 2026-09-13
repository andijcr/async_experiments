export module est:when_all;

import std;
import :future;
import :promise;
import :sync.event;
import :util.current_loop;
import :util.shared_ptr;

namespace est::detail {

// Shared state behind every est::when_all() call: counts down as each
// constituent future is accounted for - completed or abandoned,
// succeeded or failed (when_all() itself never inspects which) -
// set()ing `event` once every one of them has. A one_shot_event rather
// than a raw promise<void>/future<void> pair: `event.wait()` gives
// when_all() the future<void> it hands back to its own caller directly,
// with no separate make_promise_future() call of its own, and Mode =
// manual matches this state's own "one broadcast, however many observers
// (a caller's own `.then()`s, or a `clone()`d handle to the returned
// future<void> - see future<T>::clone()'s own doc comment) end up
// watching it" shape exactly (one_shot_event's own doc comment,
// est:sync.event). Lives exactly as long as the last constituent's own
// tracking chain (when_all_track(), below) does - `when_all()` itself
// drops its own reference the moment it returns, once every input has
// been registered.
struct when_all_state {
  explicit when_all_state(int remaining_in) : remaining(remaining_in) {}

  one_shot_event<EventResetMode::manual> event;
  int remaining;
};

// Registers `state`'s completion accounting against `input` - two
// `then_fast()` stages, not one, and each explicitly typed on a concrete
// future<T>&/future<void>& parameter rather than a generic `auto&` one.
// Either shortcut breaks correctness:
//
// - A generic `[](auto& completed) {...}` hook would incidentally
//   satisfy then()/then_fast()'s unwrapped-mode dispatch too (a template
//   parameter binds to `const T&` just as readily as to `future<T>&`),
//   which auto-propagates a failure to the (here, discarded)
//   then_fast()-returned future *without ever calling the hook at all* -
//   a failed input would never decrement `remaining`.
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
// "est::when_all(): forcing wrapped mode, and surviving abandonment" for
// the full account.
//
// then_fast(), not then(), at both stages: when_all() is exactly the
// "caller specifically knows fn is cheap and wants the already-signaled
// case to resolve without an extra loop round trip" scenario
// then_fast()'s own doc comment (est:future) describes - a no-op and a
// two-line decrement are as cheap as a then()-registered callback gets.
// For an `input` that's already ready at registration time, this lets an
// entire when_all_track() call resolve synchronously, right here, with
// no loop round trip at all - exactly like counting_event<Mode>::wait()'s
// own already-signaled fast path, the precedent then_fast() itself cites.
template <class T> void when_all_track(future<T>& input, shared_ptr<when_all_state> state) {
  input.then_fast([](future<T>&) {}).then_fast([state = std::move(state)](future<void>&) {
    if (--state->remaining == 0) {
      state->event.set();
    }
  });
}

// Builds the when_all_state its constituent hooks share - `count == 0`
// returns an empty (default-constructed) shared_ptr instead, since
// there's nothing to track at all; the two when_all() overloads below
// special-case that themselves rather than dereferencing it.
[[nodiscard]] inline auto when_all_setup(int count) -> shared_ptr<when_all_state> {
  return count == 0 ? shared_ptr<when_all_state>{}
                    : shared_ptr<when_all_state>::make(current_allocator(), count);
}

} // namespace est::detail

export namespace est {

// Resolves once every one of `futures` is accounted for - completed or
// abandoned, succeeded or failed - when_all() never reads a value or an
// exception out of any of them itself. Each future<T>& stays owned by
// the caller (when_all() only ever registers then_fast() continuations
// on it, never consumes or moves it), so once the returned future<void>
// is ready, the caller inspects failed()/get() on whichever of `futures`
// it cares about, exactly as if it had awaited each one individually.
//
// An empty pack resolves immediately, ready() the moment when_all()
// returns - there is nothing left to wait for.
//
// `state->event.wait()` is called *last*, only after every input has
// been registered - not before, alongside building `state`. Every
// registration whose input is already ready resolves synchronously right
// there, via then_fast() (when_all_track()'s own doc comment) - calling
// wait() only once all of that has already happened means its own
// already-signaled fast path (try_wait(), est:sync.event) can actually
// see the resulting signal when every input turned out to be ready,
// resolving the returned future synchronously and immediately too rather
// than by way of a redundant loop round trip: calling wait() first would
// register a waiter against a still-unsignaled event, forcing even a
// synchronously-resolved case through the same deferred, loop-enqueued
// path an already-pending input would have needed anyway.
template <class... Ts> [[nodiscard]] auto when_all(future<Ts>&... futures) -> future<void> {
  if constexpr (sizeof...(Ts) == 0) {
    return make_ready_future<void>();
  } else {
    auto state = detail::when_all_setup(static_cast<int>(sizeof...(Ts)));
    (detail::when_all_track<Ts>(futures, state), ...);
    return state->event.wait();
  }
}

// Same, for a homogeneous, dynamically-sized run of futures instead of a
// fixed argument list - a std::span<future<T>> view over caller-owned
// storage (a std::vector<future<T>>, a std::array<future<T>, N>, ...),
// never taking ownership of the futures themselves. Template argument
// deduction can't see through a container's implicit conversion to
// std::span, so a caller passing anything other than an actual
// std::span<future<T>> needs to spell one out at the call site (e.g.
// `est::when_all(std::span(my_vector))`). Same "wait() called last" shape
// as the overload above, and for the identical reason.
template <class T> [[nodiscard]] auto when_all(std::span<future<T>> futures) -> future<void> {
  if (futures.empty()) {
    return make_ready_future<void>();
  }
  auto state = detail::when_all_setup(static_cast<int>(futures.size()));
  for (auto& f : futures) {
    detail::when_all_track<T>(f, state);
  }
  return state->event.wait();
}

} // namespace est
