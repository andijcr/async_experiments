import est;
import digit_recall;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Same small allocation-counting helper est/tests/loop_tests.cpp and
// friends each keep their own copy of - matching the existing convention
// of every test file being self-contained.
class counting_resource : public memory_resource {
public:
  int allocations = 0;
  int deallocations = 0;

private:
  auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
    ++allocations;
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }

  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
    ++deallocations;
    std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
  }

  [[nodiscard]] auto do_is_equal(const memory_resource& other) const noexcept -> bool override {
    return this == &other;
  }
};

// A fake platform with a controllable clock whose sleep_until() advances
// that same fake clock instantly instead of blocking - mirrors est/tests/
// loop_tests.cpp's own fake_platform, letting a round's own timeout
// (real wall-clock seconds otherwise) resolve instantly in a test.
class fake_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return current;
  }

  void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept override {
    current = std::max(current, deadline);
  }

  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override { return 42; }

  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }

  void vprintdbg(std::string_view /*fmt*/, std::format_args /*args*/) const noexcept override {}
  void reset_loop_stall_detection() noexcept override {}
  void
  detect_loop_stall(std::chrono::steady_clock::duration /*threshold*/) const noexcept override {}

  mutable std::chrono::steady_clock::time_point current;
};

} // namespace

TEST_CASE("make_challenge() returns `length` digit characters, deterministic for a seeded rng",
          "[digit_recall]") {
  // Deliberately predictable - determinism is the whole point of this
  // test, not a bug.
  // NOLINTBEGIN(bugprone-random-generator-seed)
  std::mt19937 rng_a{1234};
  std::mt19937 rng_b{1234};
  // NOLINTEND(bugprone-random-generator-seed)

  const auto a = digit_recall::make_challenge(6, rng_a);
  const auto b = digit_recall::make_challenge(6, rng_b);

  REQUIRE(a.size() == 6);
  REQUIRE(a == b); // same seed -> same sequence of draws -> same challenge
  REQUIRE(std::ranges::all_of(a, [](char c) { return c >= '0' && c <= '9'; }));
}

TEST_CASE("round_deadline() scales with both difficulty and length", "[digit_recall]") {
  using namespace std::chrono_literals;
  constexpr auto unit = 100ms;

  REQUIRE(digit_recall::round_deadline({.difficulty = 1, .length = 4}, unit) == 400ms);
  REQUIRE(digit_recall::round_deadline({.difficulty = 2, .length = 4}, unit) == 800ms);
  REQUIRE(digit_recall::round_deadline({.difficulty = 1, .length = 8}, unit) == 800ms);
}

TEST_CASE("play_round(): a correct answer arriving before the deadline resolves `correct`, and "
          "eagerly cancels the round's own timer instead of leaving it pending",
          "[digit_recall]") {
  using namespace std::chrono_literals;
  counting_resource resource;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    est::stop_source session_stop;
    auto [prom, answer] = est::make_promise_future<std::string>();

    // A deliberately huge base_unit: if the round's own timer weren't
    // actually cancelled, run_until_idle() below would have nothing left
    // to do except sleep all the way to this far-off deadline -
    // fake_platform::sleep_until() advancing `current` that far is
    // exactly what the assertion below would catch (mirrors est/tests/
    // loop_tests.cpp's own token-aware sleep_for() cancellation test).
    auto outcome_fut = digit_recall::play_round(
        "1234", {.difficulty = 1, .length = 4}, std::move(answer), session_stop.get_token(), 1000s);
    prom.set_value("1234");
    loop.run_until_idle();

    REQUIRE(outcome_fut.ready());
    REQUIRE(outcome_fut.get() == digit_recall::RoundResult::correct);
    REQUIRE(fake.current == decltype(fake.current){}); // clock never advanced toward the deadline
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("play_round(): a wrong answer arriving before the deadline resolves `wrong`",
          "[digit_recall]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source session_stop;
  auto [prom, answer] = est::make_promise_future<std::string>();

  using namespace std::chrono_literals;
  auto outcome_fut = digit_recall::play_round(
      "1234", {.difficulty = 1, .length = 4}, std::move(answer), session_stop.get_token(), 1s);
  prom.set_value("9999");
  loop.run_until_idle();

  REQUIRE(outcome_fut.get() == digit_recall::RoundResult::wrong);
}

TEST_CASE("play_round(): typing \"quit\" resolves `quit`, not `wrong`", "[digit_recall]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source session_stop;
  auto [prom, answer] = est::make_promise_future<std::string>();

  using namespace std::chrono_literals;
  auto outcome_fut = digit_recall::play_round(
      "1234", {.difficulty = 1, .length = 4}, std::move(answer), session_stop.get_token(), 1s);
  prom.set_value("quit");
  loop.run_until_idle();

  REQUIRE(outcome_fut.get() == digit_recall::RoundResult::quit);
}

TEST_CASE("play_round(): no answer before the deadline resolves `timed_out`", "[digit_recall]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source session_stop;
  auto [prom, answer] = est::make_promise_future<std::string>();

  auto outcome_fut = digit_recall::play_round(
      "1234", {.difficulty = 1, .length = 4}, std::move(answer), session_stop.get_token(), 100ms);
  // `prom` is deliberately never fulfilled - the player never answers.
  loop.run_until_idle();

  REQUIRE(outcome_fut.get() == digit_recall::RoundResult::timed_out);
}

TEST_CASE("play_round(): the session token firing mid-round resolves `interrupted`, and the "
          "still-pending answer's later completion is a no-op",
          "[digit_recall]") {
  using namespace std::chrono_literals;
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source session_stop;
  auto [prom, answer] = est::make_promise_future<std::string>();

  auto outcome_fut = digit_recall::play_round(
      "1234", {.difficulty = 1, .length = 4}, std::move(answer), session_stop.get_token(), 1s);
  session_stop.request_stop();
  loop.run_until_idle();

  REQUIRE(outcome_fut.get() == digit_recall::RoundResult::interrupted);

  // The underlying "read" was never eagerly freed (with_stop()'s own
  // documented limitation) - completing it now, after the round already
  // resolved, must not crash or change outcome_fut's already-observed
  // result.
  prom.set_value("1234");
  loop.run_until_idle();
  REQUIRE(outcome_fut.get() == digit_recall::RoundResult::interrupted);
}
