import est;

#include <catch2/catch_test_macros.hpp>
#include <chrono>

namespace {

// Fake platform with a controllable clock, so deadline ordering can be
// tested deterministically without real sleeps.
struct fake_platform {
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  // steady_clock::time_point's default constructor isn't contractually
  // noexcept in libc++'s declaration (it just can't actually throw for
  // an arithmetic Rep), so clang-tidy conservatively flags default-
  // constructing one at static storage duration as fatal-if-it-threw.
  // NOLINTNEXTLINE(bugprone-throwing-static-initialization)
  static inline time_point current{};

  static auto now() noexcept -> time_point { return current; }
};

} // namespace

TEST_CASE("pop_ready drains entries earliest-deadline-first", "[timer]") {
  using namespace std::chrono_literals;
  fake_platform::current = fake_platform::time_point{};

  est::timer_queue<fake_platform> q;
  const auto id_late = q.schedule_at(fake_platform::current + 30s);
  const auto id_early = q.schedule_at(fake_platform::current + 10s);
  const auto id_mid = q.schedule_at(fake_platform::current + 20s);

  REQUIRE(q.next_deadline() == fake_platform::current + 10s);

  REQUIRE(q.pop_ready(fake_platform::current + 100s) == id_early);
  REQUIRE(q.pop_ready(fake_platform::current + 100s) == id_mid);
  REQUIRE(q.pop_ready(fake_platform::current + 100s) == id_late);
  REQUIRE_FALSE(q.pop_ready(fake_platform::current + 100s).has_value());
}

TEST_CASE("pop_ready returns nullopt before the deadline has passed", "[timer]") {
  using namespace std::chrono_literals;
  fake_platform::current = fake_platform::time_point{};

  est::timer_queue<fake_platform> q;
  q.schedule_at(fake_platform::current + 10s);

  REQUIRE_FALSE(q.pop_ready(fake_platform::current + 5s).has_value());
  REQUIRE(q.pop_ready(fake_platform::current + 10s).has_value());
}

TEST_CASE("cancel removes a pending entry and is idempotent-false on a second call", "[timer]") {
  using namespace std::chrono_literals;
  fake_platform::current = fake_platform::time_point{};

  est::timer_queue<fake_platform> q;
  const auto id_a = q.schedule_at(fake_platform::current + 10s);
  const auto id_b = q.schedule_at(fake_platform::current + 20s);

  REQUIRE(q.cancel(id_a));
  REQUIRE_FALSE(q.cancel(id_a));

  REQUIRE(q.pop_ready(fake_platform::current + 100s) == id_b);
  REQUIRE(q.empty());
}

TEST_CASE("schedule_after uses the platform clock's current time", "[timer]") {
  using namespace std::chrono_literals;
  fake_platform::current = fake_platform::time_point{} + 1000s;

  est::timer_queue<fake_platform> q;
  const auto id = q.schedule_after(5s);

  REQUIRE(q.next_deadline() == fake_platform::current + 5s);
  REQUIRE(q.pop_ready(fake_platform::current + 5s) == id);
}
