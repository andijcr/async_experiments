import est;
import std;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("scope_exit runs its callable on normal scope exit", "[scope_exit]") {
  bool ran = false;
  {
    est::scope_exit const guard{[&]() noexcept { ran = true; }};
    REQUIRE_FALSE(ran);
  }
  REQUIRE(ran);
}

TEST_CASE("scope_exit still runs its callable when the scope exits via an exception",
          "[scope_exit]") {
  bool ran = false;
  REQUIRE_THROWS_AS(
      [&] {
        est::scope_exit const guard{[&]() noexcept { ran = true; }};
        throw std::runtime_error("boom");
      }(),
      std::runtime_error);
  REQUIRE(ran);
}
