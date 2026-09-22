import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

// A minimal, distinguishable-from-hosted_stdcpp stub - only used to prove
// override_instance() actually retargets instance(), via a fixed,
// recognizable uptime(). Never triggers assert_failure() in these tests.
class stub_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto uptime() const noexcept -> est::platform::clock::time_point override {
    return epoch;
  }

  // Records the last deadline it was asked to sleep until, so a test can
  // confirm override_instance() actually retargets sleep_until() too -
  // never actually blocks.
  void sleep_until(est::platform::clock::time_point deadline) const noexcept override {
    last_sleep_until = deadline;
  }

  // A fixed, distinguishable value, same spirit as epoch above - a test
  // can confirm override_instance() retargets this too.
  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override {
    return random_seed;
  }

  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }

  // Records the formatted message rather than writing it anywhere, so a
  // test can confirm printdbg() actually dispatches through the
  // currently overridden instance (platform.cppm's own doc comments on
  // printdbg()/vprintdbg() - the whole point of the split from an
  // earlier, non-swappable version) instead of writing to std::cerr
  // itself.
  void vprintdbg(std::string_view fmt, std::format_args args) const noexcept override {
    try {
      last_vprintdbg_message = std::vformat(fmt, args);
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
  }

  // No-ops: none of these tests exercise est::loop's long-running-callback
  // detection - platform::interface holds no state of its own to back a
  // shared default for these, so every concrete backend, this stub
  // included, must answer them itself.
  void reset_loop_stall_detection() noexcept override {}
  void detect_loop_stall(est::platform::clock::duration /*threshold*/) const noexcept override {}

  static constexpr est::platform::clock::time_point epoch{};
  static constexpr std::uint64_t random_seed = 0xC0FFEE;
  mutable std::optional<est::platform::clock::time_point> last_sleep_until;
  mutable std::optional<std::string> last_vprintdbg_message;
};

} // namespace

TEST_CASE("override_instance retargets instance() and restores it when the guard is destroyed",
          "[platform]") {
  auto& original = est::platform::instance();
  stub_platform stub;
  {
    const auto guard = est::platform::override_instance(stub);
    REQUIRE(&est::platform::instance() == &stub);
    REQUIRE(est::platform::instance().uptime() == stub_platform::epoch);
  }
  REQUIRE(&est::platform::instance() == &original);
}

TEST_CASE("sleep_until() dispatches through the currently overridden instance", "[platform]") {
  stub_platform stub;
  const auto guard = est::platform::override_instance(stub);
  using namespace std::chrono_literals;
  const auto deadline = stub_platform::epoch + 5s;
  est::platform::instance().sleep_until(deadline);
  REQUIRE(stub.last_sleep_until == deadline);
}

TEST_CASE("get_random_seed() dispatches through the currently overridden instance", "[platform]") {
  stub_platform stub;
  const auto guard = est::platform::override_instance(stub);
  REQUIRE(est::platform::instance().get_random_seed() == stub_platform::random_seed);
}

TEST_CASE("printdbg() dispatches through the currently overridden instance", "[platform]") {
  stub_platform stub;
  const auto guard = est::platform::override_instance(stub);
  est::platform::printdbg("value is {}", 42);
  REQUIRE(stub.last_vprintdbg_message == "value is 42");
}

TEST_CASE("nested override_instance guards restore the correct previous instance", "[platform]") {
  auto& original = est::platform::instance();
  stub_platform outer;
  stub_platform inner;
  {
    const auto outer_guard = est::platform::override_instance(outer);
    REQUIRE(&est::platform::instance() == &outer);
    {
      const auto inner_guard = est::platform::override_instance(inner);
      REQUIRE(&est::platform::instance() == &inner);
    }
    REQUIRE(&est::platform::instance() == &outer);
  }
  REQUIRE(&est::platform::instance() == &original);
}

TEST_CASE("hosted_stdcpp's clock is monotonically non-decreasing", "[platform]") {
  const auto first = est::platform::instance().uptime();
  const auto second = est::platform::instance().uptime();
  REQUIRE(second >= first);
}

TEST_CASE("hosted_stdcpp's vprintdbg() writes via std::vprint_unicode without throwing",
          "[platform]") {
  // Every other printdbg()/vprintdbg() test above overrides the instance
  // with stub_platform, so none of them ever exercise hosted_stdcpp's own
  // vprintdbg() override - this is the one test that does, through the
  // real (default, non-overridden) instance. Not asserting on the actual
  // std::cerr content written - same not-practically-unit-testable stance
  // as hosted_stdcpp::assert_failure()'s own diagnostic (this file's
  // closing comment) - just confirming the real std::vprint_unicode()
  // call path runs to completion without throwing.
  est::platform::printdbg("hosted_stdcpp vprintdbg test: {}", 1);
  SUCCEED("printdbg() returned without throwing");
}

TEST_CASE("vprintdbg() handles a message longer than any fixed-size internal buffer",
          "[platform]") {
  // 96 chars > estpico's own 64-byte uart_tx_ring capacity
  // (estpico/src/platform_mps2an385.cppm). Deliberately *not* exercised
  // with a real est::loop constructed here: estpico's pump_task_loop() is
  // a process-lifetime coroutine, spawned once against whichever loop is
  // current the first time it's needed and never re-spawned afterward
  // (pump_task_started's own doc comment) - a short-lived, per-TEST_CASE
  // loop constructed and destroyed here would bind that coroutine to a
  // loop this one test case then tears down, corrupting every *later*
  // test in this same binary that also calls printdbg() - a real, if
  // narrow, "cross-loop binding hazard" worth its own follow-up rather
  // than worked around in a shared, backend-agnostic test file.
  // With no loop current, enqueue_output() instead takes its documented
  // best-effort synchronous path (a single pump_some() call, own doc
  // comment) - for a message this size on estpico specifically, that
  // means only the first ring's worth actually reaches the wire, the
  // rest silently dropped; verified separately, empirically, against a
  // real whole-program loop (docs/PLAN.md's entry for this change).
  // What this test actually guards: the oversized/truncated path itself
  // doesn't throw, crash, or corrupt state - same bar the test above
  // holds hosted_stdcpp's own vprintdbg() to.
  const std::string long_value(96, 'x');
  est::platform::printdbg("long message: {}", long_value);
  SUCCEED("printdbg() returned without throwing");
}

TEST_CASE("hosted_stdcpp's sleep_until() returns once the deadline has passed", "[platform]") {
  // A tiny (1ms) real deadline, not a fake clock: this exercises
  // hosted_stdcpp::sleep_until()'s actual std::this_thread::sleep_until()
  // call - est::loop's own tests (est/tests/loop_tests.cpp) exclusively
  // use a fake, instant sleep_until() instead, which never touches this
  // real implementation at all.
  const auto deadline = est::platform::instance().uptime() + std::chrono::milliseconds(1);
  est::platform::instance().sleep_until(deadline);
  REQUIRE(est::platform::instance().uptime() >= deadline);
}

// hosted_stdcpp::assert_failure()'s formatting isn't separately
// unit-tested: it's inlined directly into the [[noreturn]]/std::abort()
// body, the same not-practically-unit-testable situation as
// est::check()'s failure path (est/tests/check_tests.cpp).
