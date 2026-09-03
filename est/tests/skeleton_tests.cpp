import est;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the est module imports and its placeholder partition is reachable", "[skeleton]") {
  REQUIRE(est::placeholder_message() == "est walking skeleton");
}
