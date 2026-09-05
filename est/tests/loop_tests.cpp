import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Same helper as future_tests.cpp/shared_ptr_tests.cpp - counts
// allocate()/deallocate() calls so a test can assert every allocation was
// balanced, the only practical way to catch a leaked ready-queue or
// pending-timer node in a unit test without a sanitizer.
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

// A fake platform with a controllable clock whose sleep_until() advances
// that same fake clock instead of actually blocking - the seam
// est::loop's own timer-driven tests need to run instantly rather than
// for real wall-clock seconds (docs/PLAN.md, M3), extending the
// now()-only fake_platform pattern est/tests/timer_tests.cpp already
// established.
class fake_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return current;
  }

  void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept override {
    current = std::max(current, deadline);
  }

  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }

  // A no-op: nothing in these tests triggers a debug diagnostic.
  void vprintdbg(std::string_view /*fmt*/, std::format_args /*args*/) const noexcept override {}

  mutable std::chrono::steady_clock::time_point current;
};

// A platform whose now() advances by `step` on every single call - used
// to make a continuation's runtime appear to exceed the long-running-
// callback threshold without an actual real delay, so that code path gets
// exercised (docs/PLAN.md, M3's "long-running-callback detection"). Relies
// on inheriting interface::reset_loop_stall_detection()/
// detect_loop_stall()'s default implementation unchanged (docs/PLAN.md's
// "loop-stall detection moved to platform::interface" refactor) - it still
// calls now() exactly twice bracketing node.run(), the same shape
// loop::run_one() used to do directly before that logic moved onto
// platform::interface itself.
class jumping_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    const auto result = current;
    current += step;
    return result;
  }

  void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept override {
    current = std::max(current, deadline);
  }

  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }

  // A no-op, not a std::cerr write: this fake's whole purpose is
  // triggering detect_loop_stall()'s default body's diagnostic (see this
  // class's own doc comment above), which does call this - but the test
  // using it doesn't assert on the printed content (see its own doc
  // comment), so silently discarding it here just keeps test output
  // clean rather than actually writing anything.
  void vprintdbg(std::string_view /*fmt*/, std::format_args /*args*/) const noexcept override {}

  mutable std::chrono::steady_clock::time_point current;
  std::chrono::steady_clock::duration step = std::chrono::milliseconds(100);
};

} // namespace

TEST_CASE("allocator() returns the resource the loop was built with", "[loop]") {
  counting_resource resource;
  est::loop loop{&resource};
  REQUIRE(loop.allocator().resource() == &resource);
}

TEST_CASE("run_until_idle() returns immediately when there is no ready work or pending timer",
          "[loop]") {
  est::loop loop;
  loop.run_until_idle(); // must return, not hang
  SUCCEED("returned without blocking");
}

TEST_CASE("a then() continuation only runs once the loop drains, never inline", "[loop]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  bool invoked = false;
  auto chained = future.then([&](est::future<int>& state) {
    invoked = true;
    return state.get();
  });
  promise.set_value(1);
  REQUIRE_FALSE(invoked);

  loop.run_until_idle();
  REQUIRE(invoked);
  REQUIRE(chained.get() == 1);
}

TEST_CASE("run() drains ready work exactly like run_until_idle() (no I/O yet to differ on)",
          "[loop]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_value(1);
  bool invoked = false;
  auto chained = future.then([&](est::future<int>& state) {
    invoked = true;
    return state.get();
  });

  loop.run();
  REQUIRE(invoked);
  REQUIRE(chained.get() == 1);
}

TEST_CASE("stop() interrupts the current drain pass before further ready work runs", "[loop]") {
  est::loop loop;
  auto [promise_a, future_a] = est::make_promise_future<int>(loop);
  auto [promise_b, future_b] = est::make_promise_future<int>(loop);
  promise_a.set_value(1);
  promise_b.set_value(2);

  // Registered in this order, so - est::waiter_list's documented LIFO
  // order - the stop()-calling continuation for future_b is the one
  // est::loop's ready-queue actually dequeues and runs first.
  bool a_ran = false;
  auto chained_a = future_a.then([&](est::future<int>&) {
    a_ran = true;
    return 0;
  });
  auto chained_b = future_b.then([&](est::future<int>& state) {
    loop.stop();
    return state.get();
  });

  loop.run_until_idle();

  REQUIRE_FALSE(a_ran);
  REQUIRE_FALSE(chained_a.ready());
  REQUIRE(chained_b.ready());
}

TEST_CASE("sleep_for() resolves once run_until_idle() advances past the deadline", "[loop]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);

  est::loop loop;
  auto future = est::sleep_for(loop, 10s);
  REQUIRE_FALSE(future.ready());

  loop.run_until_idle();
  REQUIRE(future.ready());
}

TEST_CASE("sleep_until() resolves once run_until_idle() advances past the deadline", "[loop]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  fake.current = std::chrono::steady_clock::time_point{} + 1000s;
  const auto guard = est::platform::override_instance(fake);

  est::loop loop;
  const auto deadline = fake.current + 5s;
  auto future = est::sleep_until(loop, deadline);
  REQUIRE_FALSE(future.ready());

  loop.run_until_idle();
  REQUIRE(future.ready());
}

TEST_CASE("a then() registered on a timer-driven future runs once the timer fires", "[loop]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);

  est::loop loop;
  bool invoked = false;
  auto chained = est::sleep_for(loop, 5s).then([&] {
    invoked = true;
    return 1;
  });

  loop.run_until_idle();
  REQUIRE(invoked);
  REQUIRE(chained.get() == 1);
}

TEST_CASE("multiple pending timers all fire, earliest deadline first", "[loop]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);

  est::loop loop;
  std::vector<int> order;
  auto late = est::sleep_for(loop, 30s).then([&] { order.push_back(3); });
  auto early = est::sleep_for(loop, 10s).then([&] { order.push_back(1); });
  auto mid = est::sleep_for(loop, 20s).then([&] { order.push_back(2); });

  loop.run_until_idle();
  REQUIRE(order == std::vector{1, 2, 3});
}

TEST_CASE(
    "a continuation that appears to exceed the long-running threshold still completes normally",
    "[loop]") {
  // Exercises loop::run_one()'s long-running-callback warning path
  // (docs/PLAN.md, M3) without asserting on the printed diagnostic's
  // content - not separately unit-tested, same stance this codebase
  // already takes on platform::hosted_stdcpp::assert_failure()'s own
  // best-effort diagnostic (est/tests/check_tests.cpp).
  jumping_platform fake;
  const auto guard = est::platform::override_instance(fake);

  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_value(1);
  auto chained = future.then([](est::future<int>& state) { return state.get() + 1; });

  loop.run_until_idle();
  REQUIRE(chained.get() == 2);
}

TEST_CASE("a loop dropped with ready work and pending timers still queued frees everything, "
          "no leak",
          "[loop]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);

  counting_resource resource;
  {
    est::loop loop{&resource};
    auto [promise, future] = est::make_promise_future<int>(loop);
    promise.set_value(1);
    auto chained = future.then([](est::future<int>&) { return 0; }); // lands in ready_, never run
    auto sleeping = est::sleep_for(loop, 10s); // lands in pending_timers_, never fires
    (void)chained;
    (void)sleeping;
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}
