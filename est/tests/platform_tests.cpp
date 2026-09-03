import est;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("hosted_linux's clock is monotonically non-decreasing", "[platform]") {
  const auto first = est::platform::hosted_linux::now();
  const auto second = est::platform::hosted_linux::now();
  REQUIRE(second >= first);
}
