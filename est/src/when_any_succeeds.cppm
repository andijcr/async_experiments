export module est:when_any_succeeds;

import std;
import :future;
import :promise;
import :util.current_loop;
import :util.shared_ptr;

namespace est::detail {

// Shared state behind every est::when_any_succeeds() call: `remaining`
// counts down as each constituent that *hasn't* succeeded is accounted
// for (failed or abandoned - the two are indistinguishable from here on,
// see when_any_succeeds_track()'s own doc comment); `done` guards
// `result` against being completed more than once, since either the
// first success (immediately, whenever it happens) or the last
// remaining failure (once `remaining` reaches zero) can complete it,
// and only one of those two paths may actually ever call set_value().
struct when_any_succeeds_state {
  when_any_succeeds_state(promise<bool> result_in, int remaining_in)
      : result(std::move(result_in)), remaining(remaining_in) {}

  promise<bool> result;
  int remaining;
  bool done = false;
};

// Registers `state`'s tracking against `input` - two `then_fast()`
// stages, mirroring est::when_all()'s/est::when_any()'s own
// when_all_track()/when_any_track() (docs/wiki/Continuation-Node-Mechanism.md)
// for the identical abandonment-safety reason, with one difference: the
// first stage here isn't a pure no-op, since when_any_succeeds() needs
// to know *which way* `input` finished, not just *that* it did.
//
// It rethrows `input`'s own stored exception when `input` failed,
// translating "input failed" into "this stage's own future_state fails,
// with the same exception" - concrete_continuation<Fn, U>::run()'s
// existing callback-exception routing (est:future) does the rest,
// exactly as it would for a callback that threw on its own. `input`
// being abandoned instead of completed lands here the same way, with no
// extra code needed: abandon() (est:future) always completes its own
// downstream - this stage's own future_state - with an exception,
// whether or not the callback below ever even runs. Either way, the
// second stage only ever needs to ask "did this stage fail" - it never
// has to tell "input failed" and "input was abandoned" apart, since
// neither one is a success and that's the only distinction that matters
// from here on.
template <class T>
void when_any_succeeds_track(future<T>& input, shared_ptr<when_any_succeeds_state> state) {
  discard(input
              .then_fast([](future<T>& in) {
                if (in.ready_with_failure()) {
                  std::rethrow_exception(in.get_exception());
                }
              })
              .then_fast([state = std::move(state)](future<void>& completed) {
                if (state->done) {
                  return;
                }
                if (completed.ready_with_failure()) {
                  if (--state->remaining == 0) {
                    state->done = true;
                    state->result.set_value(false);
                  }
                } else {
                  state->done = true;
                  state->result.set_value(true);
                }
              }));
}

} // namespace est::detail

export namespace est {

// Resolves true the moment any one of `futures` succeeds, or false once
// every one of them has failed (or been abandoned - see
// when_any_succeeds_track()'s own doc comment) without any succeeding.
// Each future<T>& stays owned by the caller (when_any_succeeds() only
// ever registers then_fast() continuations on it, never consumes or
// moves it) - exactly like est::when_all()/est::when_any(), it never
// cancels whichever inputs are still running once it resolves (this
// codebase has no cancellation mechanism at all): a `true` result
// reached early, or a `false` result reached only once everything else
// has already failed, both leave any still-pending input (there can't
// be one in the `false` case, only in the early-`true` case) to finish
// in the background, unobserved.
//
// An empty pack resolves to `false` immediately: "does at least one of
// these succeed" has a well-defined answer even with nothing to check -
// there is nothing that could have succeeded.
template <class... Ts>
[[nodiscard]] auto when_any_succeeds(future<Ts>&... futures) -> future<bool> {
  auto [promise_, result] = make_promise_future<bool>();
  if constexpr (sizeof...(Ts) == 0) {
    promise_.set_value(false);
  } else {
    auto state = shared_ptr<detail::when_any_succeeds_state>::make(
        current_allocator(), std::move(promise_), static_cast<int>(sizeof...(Ts)));
    (detail::when_any_succeeds_track<Ts>(futures, state), ...);
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
// `est::when_any_succeeds(std::span(my_vector))`). An empty span
// resolves to `false` immediately, same as an empty pack above.
template <class T>
[[nodiscard]] auto when_any_succeeds(std::span<future<T>> futures) -> future<bool> {
  auto [promise_, result] = make_promise_future<bool>();
  if (futures.empty()) {
    promise_.set_value(false);
  } else {
    auto state = shared_ptr<detail::when_any_succeeds_state>::make(
        current_allocator(), std::move(promise_), static_cast<int>(futures.size()));
    for (auto& f : futures) {
      detail::when_any_succeeds_track<T>(f, state);
    }
  }
  return std::move(result);
}

} // namespace est
