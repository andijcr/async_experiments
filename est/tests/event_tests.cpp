import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Same small allocation-counting helper mutex_tests.cpp/shared_ptr_tests.cpp
// each keep their own copy of - matching the existing convention of every
// test file being self-contained.
class counting_resource : public memory_resource {
public:
  int allocations = 0;
  int deallocations = 0;

private:
  auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
    ++allocations;
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }

  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
    ++deallocations;
    std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
  }

  [[nodiscard]] auto do_is_equal(const memory_resource& other) const noexcept -> bool override {
    return this == &other;
  }
};

using est::EventResetMode;

} // namespace

TEST_CASE("counting_event<automatic>: wait() on a signaled event resolves immediately and "
          "consumes one unit",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> ev;
  ev.set(2);

  auto fut = ev.wait();
  REQUIRE(fut.ready());
  REQUIRE(ev.count() == 1);
}

TEST_CASE("counting_event<automatic>: wait() on an unsignaled event suspends until set()",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> ev;

  auto fut = ev.wait();
  REQUIRE_FALSE(fut.ready());
  REQUIRE(ev.has_waiters());

  ev.set();
  loop.run_until_idle();
  REQUIRE(fut.ready());
  REQUIRE(ev.count() == 0);
  REQUIRE_FALSE(ev.has_waiters());
}

// A long, deliberately linear sequence of independent steps, matching
// mutex_tests.cpp's own justification for the same NOLINT on similarly
// shaped scenarios.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("counting_event<automatic>: set(n) hands off to up to n queued waiters in FIFO order",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> ev;
  std::vector<int> order;

  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto waiter = [](est::loop&,
                   est::counting_event<EventResetMode::automatic>& event_ref,
                   std::vector<int>& order_ref,
                   int id) -> est::future<void> {
    co_await event_ref.wait();
    order_ref.push_back(id);
    co_return;
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

  auto fut1 = waiter(loop, ev, order, 1);
  loop.run_until_idle();
  auto fut2 = waiter(loop, ev, order, 2);
  loop.run_until_idle();
  auto fut3 = waiter(loop, ev, order, 3);
  loop.run_until_idle();

  REQUIRE(order.empty());

  ev.set(2); // only the first two queued waiters are handed a unit each
  loop.run_until_idle();

  REQUIRE(order == std::vector{1, 2});
  REQUIRE(ev.count() == 0);
  REQUIRE(ev.has_waiters()); // the third waiter is still queued

  ev.set(1);
  loop.run_until_idle();

  REQUIRE(order == std::vector{1, 2, 3});
  REQUIRE(ev.count() == 0);
  REQUIRE_FALSE(ev.has_waiters());
  REQUIRE(fut1.ready());
  REQUIRE(fut2.ready());
  REQUIRE(fut3.ready());
}

TEST_CASE("counting_event<automatic>: set(n) with no queued waiters retains the full count",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> ev;

  ev.set(5);
  REQUIRE(ev.count() == 5);
  REQUIRE_FALSE(ev.has_waiters());
}

TEST_CASE("counting_event<manual>: wait() does not consume the count - every waiter succeeds "
          "until reset()",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::manual> ev;
  ev.set();

  auto first = ev.wait();
  auto second = ev.wait();
  REQUIRE(first.ready());
  REQUIRE(second.ready());
  REQUIRE(ev.count() == 1); // unchanged by either wait()

  ev.reset();
  auto third = ev.wait();
  REQUIRE_FALSE(third.ready());
}

TEST_CASE("counting_event<manual>: set() wakes every currently queued waiter", "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::manual> ev;

  auto fut1 = ev.wait();
  auto fut2 = ev.wait();
  REQUIRE_FALSE(fut1.ready());
  REQUIRE_FALSE(fut2.ready());

  ev.set();
  loop.run_until_idle();

  REQUIRE(fut1.ready());
  REQUIRE(fut2.ready());
  REQUIRE(ev.count() == 1); // manual mode never self-clears
  REQUIRE_FALSE(ev.has_waiters());
}

TEST_CASE("reset() clears the count without affecting an already-handed-off waiter", "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::manual> ev;

  auto fut = ev.wait();
  ev.set();
  // set() has already enqueue_ready()'d fut's node onto the loop - reset()
  // right after can only affect count_, never unschedule that hand-off.
  ev.reset();
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE(ev.count() == 0);
}

TEST_CASE("counting_event() default-constructs and uses est::current_loop()", "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> ev;
  ev.set();

  auto fut = ev.wait();
  REQUIRE(fut.ready());
}

TEST_CASE("destroying a counting_event with a coroutine still queued on wait() leaks nothing",
          "[event]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    est::counting_event<EventResetMode::automatic> ev;

    // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto coro = [](est::loop&,
                   est::counting_event<EventResetMode::automatic>& event_ref) -> est::future<void> {
      co_await event_ref.wait();
      co_return; // never reached - never set()
    };
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto waiter_fut = coro(loop, ev);
    loop.run_until_idle(); // waiter suspends, queued in ev's waiters_

    REQUIRE_FALSE(waiter_fut.ready());
    REQUIRE(ev.has_waiters());
    // `ev` (and `loop`) are destroyed at the end of this scope with the
    // coroutine still suspended in co_await event_ref.wait(), never resumed.
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("binary_event<automatic>: set() is idempotent - a second set() before wait() doesn't "
          "accumulate",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::binary_event<EventResetMode::automatic> ev;

  ev.set();
  ev.set();
  REQUIRE(ev.signaled());
  REQUIRE(ev.count() == 1);

  auto fut = ev.wait();
  REQUIRE(fut.ready());
  REQUIRE_FALSE(ev.signaled()); // automatic mode: the single unit was just consumed
}

TEST_CASE("binary_event<manual>: set() stays signaled for any number of waiters until reset()",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::binary_event<EventResetMode::manual> ev;

  ev.set();
  ev.set(); // still idempotent
  REQUIRE(ev.signaled());

  auto first = ev.wait();
  auto second = ev.wait();
  REQUIRE(first.ready());
  REQUIRE(second.ready());
  REQUIRE(ev.signaled()); // manual mode never self-clears

  ev.reset();
  REQUIRE_FALSE(ev.signaled());
}

TEST_CASE("one_shot_event<manual>: a single set() satisfies every present and future waiter",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::one_shot_event<EventResetMode::manual> ev;

  auto before = ev.wait();
  REQUIRE_FALSE(before.ready());

  ev.set();
  loop.run_until_idle();
  REQUIRE(before.ready());

  auto after = ev.wait(); // still signaled - a later wait() also succeeds immediately
  REQUIRE(after.ready());
}

TEST_CASE("one_shot_event<automatic>: a single set() satisfies exactly the first waiter",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::one_shot_event<EventResetMode::automatic> ev;

  auto first = ev.wait();
  REQUIRE_FALSE(first.ready());

  ev.set();
  loop.run_until_idle();
  REQUIRE(first.ready());

  auto second = ev.wait(); // the single unit was already consumed by `first`
  REQUIRE_FALSE(second.ready());
}

TEST_CASE("one_shot_event: a second set() is a no-op, not a checked failure", "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::one_shot_event<EventResetMode::manual> ev;

  ev.set();
  ev.set(); // must not abort - independent callers may race to fire the same signal
  ev.set();
  loop.run_until_idle();

  REQUIRE(ev.signaled());
  auto fut = ev.wait();
  REQUIRE(fut.ready());
}

TEST_CASE("counting_event: max_count() reflects the constructed value, unbounded by default",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> unbounded;
  est::counting_event<EventResetMode::automatic> bounded(3);

  REQUIRE(unbounded.max_count() == std::numeric_limits<int>::max());
  REQUIRE(bounded.max_count() == 3);
}

TEST_CASE("counting_event<automatic>: set(n) saturates at max_count instead of growing without "
          "limit",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> ev(3);

  ev.set(10); // only 3 units actually fit
  REQUIRE(ev.count() == 3);

  ev.set(); // already full - a no-op, not an overflow
  REQUIRE(ev.count() == 3);
}

TEST_CASE("counting_event<automatic>: set(n) saturating at max_count still only hands off as "
          "many waiters as actually fit",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> ev(2);
  std::vector<int> order;

  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto waiter = [](est::loop&,
                   est::counting_event<EventResetMode::automatic>& event_ref,
                   std::vector<int>& order_ref,
                   int id) -> est::future<void> {
    co_await event_ref.wait();
    order_ref.push_back(id);
    co_return;
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

  auto fut1 = waiter(loop, ev, order, 1);
  loop.run_until_idle();
  auto fut2 = waiter(loop, ev, order, 2);
  loop.run_until_idle();
  auto fut3 = waiter(loop, ev, order, 3);
  loop.run_until_idle();

  ev.set(5); // max_count = 2 - only the first two queued waiters get a unit
  loop.run_until_idle();

  REQUIRE(order == std::vector{1, 2});
  REQUIRE(ev.count() == 0);  // nothing left over - the saturated amount was fully handed off
  REQUIRE(ev.has_waiters()); // the third waiter is still queued
  REQUIRE(fut1.ready());
  REQUIRE(fut2.ready());
  REQUIRE_FALSE(fut3.ready());
}

TEST_CASE("counting_event<manual>: set(n) saturates at max_count too", "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::manual> ev(2);

  ev.set(10);
  REQUIRE(ev.count() == 2);
}

TEST_CASE("binary_event<Mode>: max_count() is always 1", "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::binary_event<EventResetMode::automatic> automatic_ev;
  est::binary_event<EventResetMode::manual> manual_ev;

  REQUIRE(automatic_ev.max_count() == 1);
  REQUIRE(manual_ev.max_count() == 1);
}

TEST_CASE("counting_event<automatic>: set(n) returns the amount actually added", "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> ev;

  REQUIRE(ev.set(5) == 5);
  REQUIRE(ev.count() == 5);
}

TEST_CASE("counting_event<automatic>: set(n) returns the saturated amount, not n, once capped by "
          "max_count",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::counting_event<EventResetMode::automatic> ev(3);

  REQUIRE(ev.set(10) == 3); // only 3 units actually fit
  REQUIRE(ev.set(10) == 0); // already full - nothing left to add
}

TEST_CASE("one_shot_event: set() returns 1 for the call that actually signals, 0 after",
          "[event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::one_shot_event<EventResetMode::manual> ev;

  REQUIRE(ev.set() == 1);
  REQUIRE(ev.set() == 0);
  REQUIRE(ev.set() == 0);
}

TEST_CASE("a counting_event with no waiters can be set() and dropped with no loop current",
          "[event]") {
  // Guards counting_event::set() resolving current_loop() only once it's
  // known there's a queued waiter to hand off to, and ~counting_event()
  // checking has_waiters() before resolving current_loop() at all -
  // neither needs a loop when there's nothing queued, and must not fail
  // current_loop()'s own precondition just because none happens to be
  // registered any more.
  est::counting_event<EventResetMode::manual> ev;
  {
    est::loop loop;
    const auto loop_guard = est::make_current_loop(loop);
    ev.set(); // no waiters queued - must not need current_loop()
  } // loop_guard exits - no loop is current from here on

  REQUIRE(ev.count() == 1);
  // `ev` is destroyed at the end of this scope with no loop current -
  // must not abort.
}
