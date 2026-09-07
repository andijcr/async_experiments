import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

// Fake platform with a controllable clock, so deadline ordering can be
// tested deterministically without real sleeps. Only now() needs a real
// implementation - nothing in these tests triggers assert_failure().
class fake_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return current;
  }

  // A no-op: these tests exercise est::timer_queue directly and never
  // drive an est::loop, so sleep_until() is never actually called - every
  // est::platform::interface implementation must still provide one
  // (docs/PLAN.md, M3). See est/tests/loop_tests.cpp's own fake_platform
  // for an implementation that actually advances a fake clock instead.
  void sleep_until(std::chrono::steady_clock::time_point /*deadline*/) const noexcept override {}

  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }

  // A no-op: nothing in these tests triggers a debug diagnostic.
  void vprintdbg(std::string_view /*fmt*/, std::format_args /*args*/) const noexcept override {}

  // No-ops: these tests exercise est::timer_queue directly, never an
  // est::loop, so neither the long-running-callback path nor the
  // current-loop slot is ever touched - platform::interface holds no
  // state of its own to back a shared default for either (per review),
  // so every concrete backend, this fake included, must still answer
  // them itself.
  void reset_loop_stall_detection() noexcept override {}
  void
  detect_loop_stall(std::chrono::steady_clock::duration /*threshold*/) const noexcept override {}
  [[nodiscard]] auto get_current_loop_context() const noexcept -> est::loop* override {
    return nullptr;
  }
  void set_current_loop_context(est::loop* /*context*/) noexcept override {}

  // No `{}` needed: std::chrono::time_point's default constructor is a
  // real, user-provided constructor (time_point() : __d_(duration::zero())
  // {} in libc++'s <chrono>) that always zero-initializes, not a defaulted
  // one that would leave an automatic-storage member indeterminate -
  // confirmed directly against the pinned toolchain's header after an
  // earlier, incorrect assumption to the contrary briefly reintroduced
  // the `{}` here (see docs/PLAN.md).
  std::chrono::steady_clock::time_point current;
};

} // namespace

TEST_CASE("pop_ready drains entries earliest-deadline-first", "[timer]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);

  est::timer_queue<> q;
  const auto id_late = q.schedule_at(fake.current + 30s);
  const auto id_early = q.schedule_at(fake.current + 10s);
  const auto id_mid = q.schedule_at(fake.current + 20s);

  REQUIRE(q.next_deadline() == fake.current + 10s);

  REQUIRE(q.pop_ready(fake.current + 100s) == id_early);
  REQUIRE(q.pop_ready(fake.current + 100s) == id_mid);
  REQUIRE(q.pop_ready(fake.current + 100s) == id_late);
  REQUIRE_FALSE(q.pop_ready(fake.current + 100s).has_value());
}

TEST_CASE("pop_ready returns nullopt before the deadline has passed", "[timer]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);

  est::timer_queue<> q;
  q.schedule_at(fake.current + 10s);

  REQUIRE_FALSE(q.pop_ready(fake.current + 5s).has_value());
  REQUIRE(q.pop_ready(fake.current + 10s).has_value());
}

TEST_CASE("cancel removes a pending entry and is idempotent-false on a second call", "[timer]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);

  est::timer_queue<> q;
  const auto id_a = q.schedule_at(fake.current + 10s);
  const auto id_b = q.schedule_at(fake.current + 20s);

  REQUIRE(q.cancel(id_a));
  REQUIRE_FALSE(q.cancel(id_a));

  REQUIRE(q.pop_ready(fake.current + 100s) == id_b);
  REQUIRE(q.empty());
}

TEST_CASE("schedule_after uses the platform clock's current time", "[timer]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  fake.current = std::chrono::steady_clock::time_point{} + 1000s;
  const auto guard = est::platform::override_instance(fake);

  est::timer_queue<> q;
  const auto id = q.schedule_after(5s);

  REQUIRE(q.next_deadline() == fake.current + 5s);
  REQUIRE(q.pop_ready(fake.current + 5s) == id);
}
