import est;

#include <catch2/catch_test_macros.hpp>

// est::check()'s failure path terminates the process
// (platform::hosted_stdcpp::assert_failure() is [[noreturn]], calling
// std::abort()) - same as the <cassert> macro it replaces, there's no
// practical way to unit-test that path without process-isolation
// tooling this project doesn't have. Only the pass-through (condition
// true) path is covered here.

TEST_CASE("checks are enabled in this build", "[check]") {
  STATIC_REQUIRE(est::checks_enabled);
}

TEST_CASE("a true condition returns normally, with or without a message", "[check]") {
  est::check(true);
  est::check(true, "this message is never shown");
  SUCCEED("did not terminate");
}
