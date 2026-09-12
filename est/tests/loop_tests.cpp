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
// for real wall-clock seconds, extending the now()-only fake_platform
// pattern est/tests/timer_tests.cpp already established.
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

  // No-ops: nothing in these tests exercises the long-running-callback
  // warning path (est/tests/loop_tests.cpp's jumping_platform, below, is
  // what does) - platform::interface has no data members of its own to
  // inherit a shared implementation from, so every concrete backend,
  // this fake included, must answer these itself.
  void reset_loop_stall_detection() noexcept override {}
  void
  detect_loop_stall(std::chrono::steady_clock::duration /*threshold*/) const noexcept override {}

  mutable std::chrono::steady_clock::time_point current;
};

// A platform whose now() advances by `step` on every single call - used
// to make a continuation's runtime appear to exceed the long-running-
// callback threshold without an actual real delay, so that code path gets
// exercised. Its own reset_loop_stall_detection()/detect_loop_stall()
// below duplicate hosted_stdcpp's own implementation (platform.cppm)
// rather than inheriting a shared default - platform::interface holds no
// state of its own to back one - but the shape is unchanged: still calls
// now() exactly twice bracketing node.run(), the same measurement
// loop::run_one() itself triggers via these two calls.
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
  // triggering detect_loop_stall()'s diagnostic below (see this class's
  // own doc comment above) - but the test using it doesn't assert on the
  // printed content (see its own doc comment), so silently discarding it
  // here just keeps test output clean rather than actually writing
  // anything.
  void vprintdbg(std::string_view /*fmt*/, std::format_args /*args*/) const noexcept override {}

  void reset_loop_stall_detection() noexcept override { stall_start = now(); }

  void detect_loop_stall(std::chrono::steady_clock::duration threshold) const noexcept override {
    const auto elapsed = now() - stall_start;
    if (elapsed > threshold) {
      est::platform::printdbg(
          "stall of {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    }
  }

  mutable std::chrono::steady_clock::time_point current;
  std::chrono::steady_clock::duration step = std::chrono::milliseconds(100);
  mutable std::chrono::steady_clock::time_point stall_start;
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise_a, future_a] = est::make_promise_future<int>();
  auto [promise_b, future_b] = est::make_promise_future<int>();
  promise_a.set_value(1);
  promise_b.set_value(2);

  // Registered in this order, so - est::waiter_list's documented FIFO
  // order - the stop()-calling continuation for future_b is the one
  // est::loop's ready-queue actually dequeues and runs first.
  bool a_ran = false;
  auto chained_b = future_b.then([&](est::future<int>& state) {
    loop.stop();
    return state.get();
  });
  auto chained_a = future_a.then([&](est::future<int>&) {
    a_ran = true;
    return 0;
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
  const auto loop_guard = est::make_current_loop(loop);
  auto future = est::sleep_for(10s);
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
  const auto loop_guard = est::make_current_loop(loop);
  const auto deadline = fake.current + 5s;
  auto future = est::sleep_until(deadline);
  REQUIRE_FALSE(future.ready());

  loop.run_until_idle();
  REQUIRE(future.ready());
}

TEST_CASE("drain_pending() cancels abandoned timers so the loop can keep running safely",
          "[loop]") {
  // Guards loop::drain_pending() draining pending_timers_ without also
  // canceling the matching timer_queue entry: without that,
  // make_current_loop()'s guard abandoning a still-pending sleep_for()
  // would leave a stale deadline in timers_ that a later run_until_idle()
  // on the same, still-alive loop trips over in fire_ready_timers()'s own
  // check("loop: fired timer id missing from pending_timers_") - reached
  // here since drain_pending() (unlike the old ~loop()-only version) can
  // now run on a loop that keeps going afterward.
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);

  est::loop loop;
  {
    const auto loop_guard = est::make_current_loop(loop);
    auto future = est::sleep_for(10s); // abandoned - guard exits before it fires
    (void)future;
  } // loop_guard exits: drain_pending() destroys the pending timer node

  // The loop is still alive and still usable - run_until_idle() must not
  // find a stale deadline still sitting in the timer queue.
  loop.run_until_idle();
  SUCCEED("run_until_idle() returned without tripping the stale-timer check");
}

TEST_CASE("yield_execution() resolves once run_until_idle() drains it", "[loop]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto future = est::yield_execution();
  REQUIRE_FALSE(future.ready());

  loop.run_until_idle();
  REQUIRE(future.ready());
}

TEST_CASE("yield_execution() lets already-ready work run first", "[loop]") {
  // Issue #45: yield_execution() is sugar over a zero-duration sleep_for()
  // (est:promise) specifically so it lands in pending_timers_ rather than
  // ready_ - run_impl() (est:loop) always fully drains ready_ before ever
  // checking pending_timers_, so anything already ready when
  // yield_execution() is called runs first, however many rounds that
  // takes (drain_ready() loops until ready_ is empty, not just once).
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::vector<int> order;

  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(1);
  auto already_ready = future.then([&](est::future<int>&) {
    order.push_back(1);
    return 0;
  });

  auto yielded = est::yield_execution().then([&] { order.push_back(2); });

  loop.run_until_idle();
  REQUIRE(order == std::vector{1, 2});
}

TEST_CASE("a then() registered on a timer-driven future runs once the timer fires", "[loop]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);

  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  bool invoked = false;
  auto chained = est::sleep_for(5s).then([&] {
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
  const auto loop_guard = est::make_current_loop(loop);
  std::vector<int> order;
  auto late = est::sleep_for(30s).then([&] { order.push_back(3); });
  auto early = est::sleep_for(10s).then([&] { order.push_back(1); });
  auto mid = est::sleep_for(20s).then([&] { order.push_back(2); });

  loop.run_until_idle();
  REQUIRE(order == std::vector{1, 2, 3});
}

TEST_CASE(
    "a continuation that appears to exceed the long-running threshold still completes normally",
    "[loop]") {
  // Exercises loop::run_one()'s long-running-callback warning path
  // without asserting on the printed diagnostic's content - not
  // separately unit-tested, same stance this codebase
  // already takes on platform::hosted_stdcpp::assert_failure()'s own
  // best-effort diagnostic (est/tests/check_tests.cpp).
  jumping_platform fake;
  const auto guard = est::platform::override_instance(fake);

  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
    const auto loop_guard = est::make_current_loop(loop);
    auto [promise, future] = est::make_promise_future<int>();
    promise.set_value(1);
    auto chained = future.then([](est::future<int>&) { return 0; }); // lands in ready_, never run
    auto sleeping = est::sleep_for(10s); // lands in pending_timers_, never fires
    (void)chained;
    (void)sleeping;
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("destroying a loop with a coroutine co_await-ing yield_execution() still pending "
          "leaks nothing",
          "[loop]") {
  // Same hazard as est:sync.event's detail::promise_resume_node (est/tests/mutex_tests.cpp,
  // "destroying a mutex with a coroutine co_await-ing lock() still
  // pending leaks nothing") - a coroutine suspended via co_await holds
  // its own reference to yield_execution()'s future_state<void> (the
  // future<void> temporary co_await awaits is spilled into the
  // coroutine's own frame across the suspension), so dropping only
  // detail::promise_resume_node's own reference (via ~loop()'s drain of
  // ready_) would leave that future_state - and the coroutine frame
  // keeping it alive - with nowhere left to go, unless abandon() actually
  // completes the promise instead of silently dropping it. See
  // detail::promise_resume_node's own doc comment (est/src/promise.cppm)
  // for the full reasoning.
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto coro = [](est::loop&) -> est::future<void> {
      co_await est::yield_execution();
      co_return; // never reached - loop is destroyed before this ever drains
    };
    auto fut = coro(loop);

    REQUIRE_FALSE(fut.ready());
    (void)fut;
    // `loop` is destroyed at the end of this scope with the coroutine
    // still suspended in co_await yield_execution(), never resumed.
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("destroying a loop with a coroutine co_await-ing sleep_for() still pending leaks "
          "nothing",
          "[loop]") {
  // Issue #50: sleep_for()/sleep_until()'s old node (concrete_timer_node<Fn>,
  // wrapping a closure that itself captured the promise) couldn't complete
  // that promise on abandonment - a type-erased Fn gave destroy() no way
  // to know it held a promise<void> at all. Fixed by detail::
  // sleep_resume_node, which holds the promise<void> directly - same
  // hazard, same fix shape as yield_execution()'s own leak test just
  // above and mutex's "destroying a mutex with a coroutine co_await-ing
  // acquire() still pending leaks nothing" (est/tests/mutex_tests.cpp).
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);

  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto coro = [](est::loop&) -> est::future<void> {
      co_await est::sleep_for(10s);
      co_return; // never reached - loop is destroyed before the timer fires
    };
    auto fut = coro(loop);

    REQUIRE_FALSE(fut.ready());
    (void)fut;
    // `loop` is destroyed at the end of this scope with the coroutine
    // still suspended in co_await sleep_for(10s), the timer never having
    // fired.
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

// Issue #30: est::make_current_loop()/current_loop()
// (est:util.current_loop) and the no-loop sugar built on current_loop().
// est::check()'s own failure path (make_current_loop()-ing a second loop
// while one is already current, or calling current_loop() with none
// registered at all - there is no fallback) isn't unit-testable in this
// codebase - it terminates the process, same as every other checked
// precondition (est/tests/check_tests.cpp's own doc comment) - so only
// the happy path is covered here.

TEST_CASE("current_loop() returns whichever loop called make_current_loop()", "[loop]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  REQUIRE(&est::current_loop() == &loop);
}

TEST_CASE("make_promise_future<T>() with no loop argument uses est::current_loop()", "[loop]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(42);
  REQUIRE(future.ready());
  REQUIRE(future.get() == 42);
}

TEST_CASE("make_ready_future<T>(args...) returns an already-ready future constructed from args",
          "[loop]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  auto future = est::make_ready_future<int>(42);
  REQUIRE(future.ready());
  REQUIRE(future.get() == 42);
}

TEST_CASE("make_ready_future<T>(args...) forwards every argument to T's constructor", "[loop]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  auto future = est::make_ready_future<std::string>(std::size_t{3}, 'x');
  REQUIRE(future.ready());
  REQUIRE(future.get() == "xxx");
}

TEST_CASE("make_ready_future<void>() returns an already-ready future<void>", "[loop]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  auto future = est::make_ready_future<void>();
  REQUIRE(future.ready());
}

TEST_CASE("sleep_for()/sleep_until()/yield_execution() with no loop argument use "
          "est::current_loop()",
          "[loop]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);

  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  auto slept_for = est::sleep_for(10s);
  auto slept_until = est::sleep_until(fake.current + 5s);
  auto yielded = est::yield_execution();
  REQUIRE_FALSE(slept_for.ready());
  REQUIRE_FALSE(slept_until.ready());
  REQUIRE_FALSE(yielded.ready());

  loop.run_until_idle();
  REQUIRE(slept_for.ready());
  REQUIRE(slept_until.ready());
  REQUIRE(yielded.ready());
}
