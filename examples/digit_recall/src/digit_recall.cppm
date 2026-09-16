export module digit_recall;

import est;
import std;

// digit_recall: a small terminal reflex game demonstrating est::stop_token
// (est:sync.stop_token) and est::with_stop<T>() (est:with_stop) side by
// side, in their two genuinely different shapes:
//   - the per-round timeout is cancelled *eagerly*, via a token-aware
//     est::sleep_for() - the still-pending timer is actually pulled out
//     of the loop's timer queue the moment it's moot, not just
//     "stopped watching" (loop::cancel_timer(), est:loop).
//   - the whole session's time budget cuts a round short via
//     est::with_stop(), which only stops the *caller* from waiting - the
//     underlying read (real, non-blocking stdin polling in production,
//     digit_recall_io.cppm) keeps running, unobserved, exactly matching
//     that function's own documented limitation.
// See docs/wiki/Coroutines.md's "Cancellation: stop_token vs.
// abandonment" section for the general distinction these two mirror.
export namespace digit_recall {

// How long the digit strings get (`length`) and the per-digit time
// budget (`difficulty`) - both only ever grow across a session (see
// main.cpp's own difficulty-progression rule). Starting values chosen
// to be comfortably beatable by a human typing at a normal pace.
struct level {
  int difficulty = 1;
  std::size_t length = 4;
};

// Builds the next challenge: `length` random digits drawn from `rng` -
// templated on the generator (rather than always reaching for
// std::random_device) so tests can supply a seeded, deterministic one.
template <class URBG>
[[nodiscard]] auto make_challenge(std::size_t length, URBG& rng) -> std::string {
  static constexpr std::string_view digits = "0123456789";
  std::uniform_int_distribution<std::size_t> pick(0, digits.size() - 1);
  std::string challenge(length, '0');
  for (char& c : challenge) {
    // pick(rng) is constructed with bounds [0, digits.size() - 1] just
    // above, so this is always in range - never actually unchecked.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    c = digits[pick(rng)];
  }
  return challenge;
}

// The per-round time budget: a longer or harder challenge gets
// proportionally more time, not a fixed window regardless of either -
// `base_unit` is the "comfortable time to type one digit" constant the
// whole curve scales from.
[[nodiscard]] constexpr auto round_deadline(level lvl, est::loop::clock::duration base_unit)
    -> est::loop::clock::duration {
  return base_unit * (lvl.difficulty * static_cast<int>(lvl.length));
}

enum class RoundResult : std::uint8_t { correct, wrong, timed_out, interrupted, quit };

// Plays one round: races `answer` (the player's in-flight typed line)
// against this round's own timeout, with the whole race additionally
// cut short if `session_token` fires first.
//
// session_token taken by value, not const&: this is a coroutine (co_await
// below), and cppcoreguidelines-avoid-reference-coroutine-parameters
// flags a coroutine reference parameter regardless of whether the
// referent actually outlives it - est::stop_token is just a shared_ptr
// handle, so a copy is cheap and sidesteps the question entirely rather
// than relying on the check's blind spot for a pointer instead.
[[nodiscard]] auto play_round(std::string challenge,
                              level lvl,
                              est::future<std::string> answer,
                              est::stop_token session_token,
                              est::loop::clock::duration base_unit) -> est::future<RoundResult> {
  auto guarded_answer = est::with_stop(std::move(answer), session_token);

  // Fresh per round: request_stop() below is the *only* thing this
  // token ever fires, purely to reclaim this round's own timer early
  // once it's moot - never exposed past this function.
  est::stop_source round_stop;
  auto timeout = est::sleep_for(round_deadline(lvl, base_unit), round_stop.get_token());

  co_await est::when_any(guarded_answer, timeout);

  if (timeout.ready_with_value()) {
    co_return RoundResult::timed_out;
  }

  // guarded_answer won the race - the round's own timeout is moot now;
  // free it for real instead of leaving it to fire uselessly later.
  round_stop.request_stop();

  if (guarded_answer.ready_with_failure()) {
    // Either session_token fired mid-round, or the underlying read
    // itself failed (real stdin EOF, in production) - both just mean
    // "this round can't continue," so both funnel through operation_
    // cancelled/whatever exception guarded_answer carries without this
    // function needing to tell them apart.
    co_return RoundResult::interrupted;
  }

  auto line = std::move(guarded_answer).get();
  if (line == "quit") {
    co_return RoundResult::quit;
  }
  co_return line == challenge ? RoundResult::correct : RoundResult::wrong;
}

} // namespace digit_recall
