import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

// A minimal, distinguishable-from-hosted_stdcpp stub - only used to prove
// override_instance() actually retargets instance(), via a fixed,
// recognizable now(). Never triggers assert_failure() in these tests.
class stub_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return epoch;
  }

  // Records the last deadline it was asked to sleep until, so a test can
  // confirm override_instance() actually retargets sleep_until() too -
  // never actually blocks.
  void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept override {
    last_sleep_until = deadline;
  }

  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }

  static constexpr std::chrono::steady_clock::time_point epoch{};
  mutable std::optional<std::chrono::steady_clock::time_point> last_sleep_until;
};

} // namespace

TEST_CASE("override_instance retargets instance() and restores it when the guard is destroyed",
          "[platform]") {
  auto& original = est::platform::instance();
  stub_platform stub;
  {
    const auto guard = est::platform::override_instance(stub);
    REQUIRE(&est::platform::instance() == &stub);
    REQUIRE(est::platform::instance().now() == stub_platform::epoch);
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
  const auto first = est::platform::instance().now();
  const auto second = est::platform::instance().now();
  REQUIRE(second >= first);
}

TEST_CASE("hosted_stdcpp's sleep_until() returns once the deadline has passed", "[platform]") {
  // A tiny (1ms) real deadline, not a fake clock: this exercises
  // hosted_stdcpp::sleep_until()'s actual std::this_thread::sleep_until()
  // call - est::loop's own tests (est/tests/loop_tests.cpp) exclusively
  // use a fake, instant sleep_until() instead, which never touches this
  // real implementation at all.
  const auto deadline = est::platform::instance().now() + std::chrono::milliseconds(1);
  est::platform::instance().sleep_until(deadline);
  REQUIRE(est::platform::instance().now() >= deadline);
}

// hosted_stdcpp::assert_failure()'s formatting isn't separately unit-tested:
// unlike an earlier version of this file, it's no longer split out into a
// standalone, testable helper - it's inlined directly into the
// [[noreturn]]/std::abort() body, the same not-practically-unit-testable
// situation as est::check()'s failure path (est/tests/check_tests.cpp).
