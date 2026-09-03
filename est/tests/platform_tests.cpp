import est;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("hosted_linux's clock is monotonically non-decreasing", "[platform]") {
  const auto first = est::platform::hosted_linux::now();
  const auto second = est::platform::hosted_linux::now();
  REQUIRE(second >= first);
}

TEST_CASE("hosted_linux's critical section is callable and side-effect-free", "[platform]") {
  est::platform::hosted_linux::enter_critical_section();
  est::platform::hosted_linux::leave_critical_section();
  SUCCEED("enter/leave did not throw or crash");
}
