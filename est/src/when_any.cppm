export module est:when_any;

import std;
import :check;
import :future;
import :sync.event;
import :util.current_loop;
import :util.shared_ptr;

namespace est::detail {

// Registers `event`'s completion signal against `input` - two
// `then_fast()` stages, not one, and each explicitly typed on a concrete
// future<T>&/future<void>& parameter rather than a generic `auto&` one -
// identical reasoning to est::when_all()'s own when_all_track()
// (docs/wiki/Continuation-Node-Mechanism.md, "est::when_all(): forcing
// wrapped mode, and surviving abandonment"): a generic hook would
// incidentally land in then_fast()'s unwrapped mode, silently skipping a
// failed input; a hook registered directly on `input` would silently
// skip an *abandoned* one instead, since concrete_continuation<Fn,
// U>::abandon() (est:future) only ever completes its own (here,
// discarded) downstream, never `fn_`. The two-stage chain sidesteps
// both: the first stage is a no-op that reliably produces a future<void>
// completing whenever `input` does, whichever way that happens; the
// real signal - a single `event->set()` call - lives in the second
// stage instead.
//
// Unlike when_all_track()'s state, `event` needs no counter at all:
// one_shot_event<Mode>::set() is already safe to call redundantly from
// as many racing completions as end up reaching it (its own doc comment,
// est:sync.event, calls out exactly this - "two unrelated cancellation
// sources racing to fire the same one-shot signal" - as the reason it
// doesn't require callers to coordinate first). Every input but the
// first to finish calls set() on an already-signaled event and is a
// no-op.
template <class T>
void when_any_track(future<T>& input, shared_ptr<one_shot_event<EventResetMode::manual>> event) {
  input.then_fast([](future<T>&) {}).then_fast([event = std::move(event)](future<void>&) {
    event->set();
  });
}

} // namespace est::detail

export namespace est {

// Resolves the moment *any one* of `futures` is accounted for - completed
// or abandoned, succeeded or failed - est::when_any() never reads a value
// or an exception out of any of them itself, and never cancels the ones
// that haven't finished yet (this codebase has no cancellation mechanism
// at all): they simply keep running to completion in the background,
// exactly as any other unobserved future always does, with nothing left
// watching them once when_any() itself has returned. Each future<T>&
// stays owned by the caller (when_any() only ever registers then_fast()
// continuations on it, never consumes or moves it) - once the returned
// future<void> is ready, the caller inspects ready()/ready_with_failure()/get() on
// whichever of `futures` it cares about to find out which one actually
// won (or which ones, if more than one happened to finish together -
// "any one" doesn't promise there's exactly one).
//
// At least one future is required - "any one of zero" has nothing that
// could ever complete it, so an empty call is a compile error here
// (checked statically, since the pack size is known at compile time);
// see the std::span overload below for the equivalent runtime-sized
// case.
//
// `event->wait()` is called *last*, only after every input has been
// registered - not before, alongside building `event` - for the
// identical reason est::when_all() calls its own event's wait() last
// (see its own doc comment, est/src/when_all.cppm): every registration
// whose input is already ready resolves synchronously right here, via
// then_fast() - calling wait() only once all of that has already
// happened lets its own already-signaled fast path (try_wait(),
// est:sync.event) resolve the returned future synchronously too, when
// any input turned out to already be ready, rather than forcing even
// that case through a redundant loop round trip.
template <class... Ts> [[nodiscard]] auto when_any(future<Ts>&... futures) -> future<void> {
  static_assert(sizeof...(Ts) > 0, "when_any() requires at least one future");
  auto event = shared_ptr<one_shot_event<EventResetMode::manual>>::make(current_allocator());
  (detail::when_any_track<Ts>(futures, event), ...);
  return event->wait();
}

// Same, for a homogeneous, dynamically-sized run of futures instead of a
// fixed argument list - a std::span<future<T>> view over caller-owned
// storage (a std::vector<future<T>>, a std::array<future<T>, N>, ...),
// never taking ownership of the futures themselves. Template argument
// deduction can't see through a container's implicit conversion to
// std::span, so a caller passing anything other than an actual
// std::span<future<T>> needs to spell one out at the call site (e.g.
// `est::when_any(std::span(my_vector))`). An empty span is a checked
// precondition violation, not a well-defined empty case the way
// est::when_all()'s own span overload treats it - "any one of zero" is
// meaningless, not vacuously true - checked at runtime here rather than
// compile time only because a std::span's size isn't visible to the
// compiler the way a parameter pack's is.
template <class T> [[nodiscard]] auto when_any(std::span<future<T>> futures) -> future<void> {
  check(!futures.empty(), "when_any(): futures must not be empty");
  auto event = shared_ptr<one_shot_event<EventResetMode::manual>>::make(current_allocator());
  for (auto& f : futures) {
    detail::when_any_track<T>(f, event);
  }
  return event->wait();
}

} // namespace est
