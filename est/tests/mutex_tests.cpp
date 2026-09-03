import est;

#include <catch2/catch_test_macros.hpp>
#include <functional>

namespace {

// Fake platform for deterministic testing: counts critical-section
// enter/leave calls, and can be told to invoke a callback from within
// enter() to simulate a timer/IO completion "interrupt" arriving mid-
// critical-section - the scenario est::basic_mutex exists to guard
// against (docs/PLAN.md, M1), even though hosted_linux itself doesn't
// model real interrupts yet.
struct fake_platform {
  static inline int enter_count = 0;
  static inline int leave_count = 0;
  static inline std::function<void()> on_enter;

  static void enter_critical_section() noexcept {
    ++enter_count;
    if (on_enter) {
      auto callback = std::move(on_enter);
      on_enter = nullptr; // avoid recursing if the callback itself locks
      callback();
    }
  }

  static void leave_critical_section() noexcept { ++leave_count; }

  static void reset() noexcept {
    enter_count = 0;
    leave_count = 0;
    on_enter = nullptr;
  }
};

} // namespace

TEST_CASE("lock/unlock go through the platform's critical section exactly once each", "[mutex]") {
  fake_platform::reset();
  est::basic_mutex<fake_platform> m;

  REQUIRE_FALSE(m.locked());
  m.lock();
  REQUIRE(m.locked());
  REQUIRE(fake_platform::enter_count == 1);
  REQUIRE(fake_platform::leave_count == 0);

  m.unlock();
  REQUIRE_FALSE(m.locked());
  REQUIRE(fake_platform::leave_count == 1);
}

TEST_CASE("waiter list is LIFO and payload-free", "[mutex]") {
  fake_platform::reset();
  est::basic_mutex<fake_platform> m;
  est::mutex_waiter a;
  est::mutex_waiter b;
  est::mutex_waiter c;

  REQUIRE_FALSE(m.has_waiters());

  m.lock();
  m.enqueue(a);
  m.enqueue(b);
  m.enqueue(c);
  m.unlock();

  REQUIRE(m.has_waiters());

  m.lock();
  REQUIRE(m.dequeue() == &c);
  REQUIRE(m.dequeue() == &b);
  REQUIRE(m.dequeue() == &a);
  REQUIRE(m.dequeue() == nullptr);
  m.unlock();

  REQUIRE_FALSE(m.has_waiters());
}

TEST_CASE(
    "waiter-list operations stay consistent under a simulated reentrant critical-section call",
    "[mutex][platform]") {
  fake_platform::reset();
  est::basic_mutex<fake_platform> m;
  est::mutex_waiter from_mainline;
  est::mutex_waiter from_simulated_interrupt;

  // Simulate a timer/IO completion "interrupt" arriving while mainline
  // code is inside the critical section taken by lock(): the fake
  // platform's enter_critical_section() hook enqueues its own waiter
  // before mainline continues. hosted_linux doesn't actually mask
  // anything (M1 scope), so this exercises the same mutex API a future
  // bare-metal backend must keep safe under a real interrupt.
  fake_platform::on_enter = [&] { m.enqueue(from_simulated_interrupt); };

  m.lock();
  m.enqueue(from_mainline);
  m.unlock();

  REQUIRE(m.dequeue() == &from_mainline);
  REQUIRE(m.dequeue() == &from_simulated_interrupt);
  REQUIRE(m.dequeue() == nullptr);
}
