import est;
import std;

#include <catch2/catch_test_macros.hpp>

// No platform::override_instance() needed in this file: test_main.cpp
// already installs estext::hosted_stdcpp for the whole binary, and
// est::jitter only ever calls platform::instance().get_random_seed()
// once, at construction - never now()/sleep_until(), so the real
// hosted_stdcpp backend is fine to exercise directly here.

TEST_CASE("jitter() stays within [-max_jitter, +max_jitter]", "[jitter]") {
  using namespace std::chrono_literals;
  est::jitter jit(100ms);
  for (int i = 0; i < 200; ++i) {
    const auto offset = jit();
    REQUIRE(offset >= -100ms);
    REQUIRE(offset <= 100ms);
  }
}

TEST_CASE("jitter() with a zero max_jitter always returns zero", "[jitter]") {
  est::jitter jit(std::chrono::steady_clock::duration::zero());
  for (int i = 0; i < 10; ++i) {
    REQUIRE(jit() == std::chrono::steady_clock::duration::zero());
  }
}

TEST_CASE("jitter() draws more than one distinct value across many calls", "[jitter]") {
  using namespace std::chrono_literals;
  est::jitter jit(50ms);
  std::set<std::chrono::steady_clock::duration::rep> seen;
  for (int i = 0; i < 100; ++i) {
    seen.insert(jit().count());
  }
  // Not a fixed value or a two-value alternation - a real uniform spread.
  REQUIRE(seen.size() > 2);
}
