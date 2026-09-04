import est;
import std;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("hosted_linux's clock is monotonically non-decreasing", "[platform]") {
  const auto first = est::platform::hosted_linux::now();
  const auto second = est::platform::hosted_linux::now();
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
