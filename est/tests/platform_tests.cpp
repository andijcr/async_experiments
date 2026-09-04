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

TEST_CASE("format_assertion_message includes location; an empty message adds no extra "
          "'assertion failed: ' separator",
          "[platform]") {
  const auto location = std::source_location::current();
  const auto formatted = est::platform::hosted_linux::format_assertion_message("", location);
  REQUIRE(formatted.contains(location.file_name()));
  REQUIRE(formatted.contains(location.function_name()));
  REQUIRE(formatted.contains(std::to_string(location.line())));
  REQUIRE_FALSE(formatted.contains("assertion failed: "));
}

TEST_CASE("format_assertion_message includes a non-empty message", "[platform]") {
  const auto formatted = est::platform::hosted_linux::format_assertion_message(
      "custom message", std::source_location::current());
  REQUIRE(formatted.contains("assertion failed: custom message"));
}
