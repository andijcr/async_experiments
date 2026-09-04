import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

// A minimal, distinguishable-from-hosted_linux stub - only used to prove
// override_instance() actually retargets instance(), via a fixed,
// recognizable now(). Never triggers assert_failure() in these tests.
class stub_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return epoch;
  }

  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }

  static constexpr std::chrono::steady_clock::time_point epoch{};
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

TEST_CASE("hosted_linux's clock is monotonically non-decreasing", "[platform]") {
  const auto first = est::platform::instance().now();
  const auto second = est::platform::instance().now();
  REQUIRE(second >= first);
}

// hosted_linux::assert_failure()'s formatting isn't separately unit-tested:
// unlike an earlier version of this file, it's no longer split out into a
// standalone, testable helper - it's inlined directly into the
// [[noreturn]]/std::abort() body, the same not-practically-unit-testable
// situation as est::check()'s failure path (est/tests/check_tests.cpp).
