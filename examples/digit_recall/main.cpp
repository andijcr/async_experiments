import est;
import estext;
import digit_recall;
import digit_recall_io;
import std;

// EXIT_SUCCESS/EXIT_FAILURE and the POSIX fcntl()/O_NONBLOCK/STDIN_FILENO
// setup below are macros/plain declarations `import std;` doesn't carry
// (same reasoning as examples/spreadsheet/main.cpp), so this stays a
// classic #include.
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

// "Comfortable time to type one digit" - digit_recall::round_deadline()
// scales this by difficulty*length for each round's actual timeout.
constexpr auto base_unit = 600ms;
// The "general stop" cancellation point: once this elapses, the whole
// session ends - see play_session()'s own doc comment for how that
// reaches a round that's still waiting on the player.
constexpr auto session_time_limit = 90s;

// Drives an entire session: builds a fresh challenge each round, plays
// it via digit_recall::play_round(), applies the outcome (score tally,
// difficulty/length growth), and loops until the round result says to
// stop. Not unit tested itself - a thin orchestration/printing driver
// over digit_recall::play_round(), which is - mirrors examples/
// spreadsheet/main.cpp's own run_server(), tested the identical way:
// the reusable logic lives in a real module with real tests, this loop
// doesn't.
//
// Takes session_stop by pointer, not reference, for the same reason
// spreadsheet/main.cpp's run_server() takes sheet_instance by pointer -
// cppcoreguidelines-avoid-reference-coroutine-parameters flags a
// coroutine reference parameter regardless of whether the referent
// actually outlives it (main()'s session_stop genuinely does, for as
// long as loop.run() is running); a pointer says so without relying on
// the check's blind spot for references specifically.
auto play_session(est::stop_source* session_stop) -> est::future<void> {
  digit_recall::level lvl;
  int score = 0;
  std::mt19937 rng{std::random_device{}()};

  for (;;) {
    auto challenge = digit_recall::make_challenge(lvl.length, rng);
    std::println("\n[{} digits, {}x speed] {}", lvl.length, lvl.difficulty, challenge);
    std::println("(type it back, or \"quit\")");

    const auto outcome = co_await digit_recall::play_round(
        challenge, lvl, digit_recall::read_line_async(), session_stop->get_token(), base_unit);

    switch (outcome) {
      case digit_recall::RoundResult::correct:
        ++score;
        std::println("correct! score: {}", score);
        // Alternates which axis gets harder, rather than growing both at
        // once - either is a valid ramp; this one keeps the two curves
        // (string length, per-digit time pressure) independently visible
        // across a session instead of always compounding together.
        if (score % 2 == 0) {
          ++lvl.difficulty;
        } else {
          ++lvl.length;
        }
        continue;
      case digit_recall::RoundResult::wrong:
        std::println("wrong - it was {}. final score: {}", challenge, score);
        co_return;
      case digit_recall::RoundResult::timed_out:
        std::println("too slow! final score: {}", score);
        co_return;
      case digit_recall::RoundResult::quit:
        std::println("bye! final score: {}", score);
        co_return;
      case digit_recall::RoundResult::interrupted:
        std::println("\nsession over. final score: {}", score);
        co_return;
    }
  }
}

} // namespace

auto main() -> int {
  // A concrete platform::interface isn't installed automatically just by
  // `import est;` - this program's own decision to make (examples/
  // hello_world/main.cpp, examples/spreadsheet/main.cpp do the same).
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  // Line-buffer stdout - see examples/spreadsheet/main.cpp's own doc
  // comment on why: fully-buffered-when-not-a-tty is the normal case for
  // a piped/redirected run, and without this every std::println() below
  // would sit invisible in libc's buffer instead of appearing promptly.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  // digit_recall::read_line_async() polls stdin with a raw, non-blocking
  // ::read() rather than ever blocking the one thread est::loop runs on
  // - this is the one-time setup that makes that safe (digit_recall_io.
  // cppm's own doc comment has the full story on why).
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
  const int flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
  ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
  // NOLINTEND(cppcoreguidelines-pro-type-vararg)

  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  est::stop_source session_stop;       // fires once the session's overall time
                                       // budget elapses - see below
  est::stop_source session_timer_stop; // controls the timer driving session_stop
                                       // itself, so it can be reclaimed early

  try {
    // The "general stop" of the whole game: once the session's overall
    // time budget elapses, this token-aware sleep_for()'s own then() fires
    // session_stop's token - play_round()'s own with_stop(answer,
    // session_token) observes it even mid-round, ending the current round
    // (and the session) the moment it's noticed rather than waiting for
    // whatever round is in flight to resolve on its own. Unwrapped (no
    // captured future<void>&, per then()'s own dispatch rules), so it's
    // skipped entirely - not run with a stale/cancelled result - if
    // session_timer_stop cancels this timer first, below. est::spawn()
    // (issue #58): explicit, loop-owned ownership of this chain's own
    // downstream future<void> instead of a bare discarded handle.
    est::spawn(
        est::sleep_for(session_time_limit, session_timer_stop.get_token()).then([&session_stop] {
          session_stop.request_stop();
        }));

    // session itself is kept (not spawn()ed): the block below registers a
    // further .then() on it, so it has to stay a live handle, not
    // something handed off to spawn()'s own tracking.
    auto session = play_session(&session_stop);
    // Once the session itself ends (quit, a wrong/timed-out round, or the
    // session time limit already firing) the session-length timer above is
    // moot - eagerly reclaim it (loop::cancel_timer(), the same mechanism
    // the fast path above already relies on) and ask the loop to stop,
    // rather than leaving loop.run() blocked on a real timer that no
    // longer matters. est::spawn() again for this chain's own downstream
    // future<void>, same reasoning as above.
    est::spawn(session.then([&loop, &session_timer_stop](est::future<void>&) {
      session_timer_stop.request_stop();
      loop.stop();
    }));
    loop.run();
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
