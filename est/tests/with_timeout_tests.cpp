import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Same helper as future_tests.cpp/loop_tests.cpp - counts
// allocate()/deallocate() calls so a test can assert every allocation was
// balanced, and (combined with the fake clock below) that a cancelled
// deadline timer was actually reclaimed early, not merely left pending.
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

// Same fake platform as est/tests/loop_tests.cpp's own - a controllable
// clock whose interruptible_sleep_until() actually advances `current`
// (rather than blocking for real), so a pending timer's deadline can be
// reached synchronously within a single run_until_idle() call.
class fake_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto uptime() const noexcept -> est::platform::clock::time_point override {
    return current;
  }

  void interruptible_sleep_until(est::platform::clock::time_point deadline) noexcept override {
    current = std::max(current, deadline);
  }

  void wake(est::platform::interface::WakeId /*id*/) noexcept override {}
  void wake_all() noexcept override {}

  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override { return 42; }

  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }

  void vprintdbg(std::string_view /*fmt*/, std::format_args /*args*/) const noexcept override {}

  void reset_loop_stall_detection() noexcept override {}
  void detect_loop_stall(est::platform::clock::duration /*threshold*/) const noexcept override {}

  mutable est::platform::clock::time_point current;
};

} // namespace

TEST_CASE("with_timeout(): the operation completing before the deadline forwards its own value",
          "[with_timeout]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [prom, operation] = est::make_promise_future<int>();

  auto fut = est::with_timeout(std::move(operation), 10s);
  REQUIRE_FALSE(fut.ready());

  prom.set_value(42);
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE_FALSE(fut.ready_with_failure());
  REQUIRE(fut.get() == 42);
}

TEST_CASE("with_timeout(): the operation completing before the deadline forwards its own "
          "failure unchanged",
          "[with_timeout]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [prom, operation] = est::make_promise_future<int>();

  auto fut = est::with_timeout(std::move(operation), 10s);
  prom.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE(fut.ready_with_failure());
  REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("with_timeout(): the deadline firing before the operation completes resolves with "
          "operation_timed_out, and the operation's own later completion is a no-op",
          "[with_timeout]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [prom, operation] = est::make_promise_future<int>();

  auto fut = est::with_timeout(std::move(operation), 10s);
  REQUIRE_FALSE(fut.ready());

  loop.run_until_idle(); // nothing else pending - advances the fake clock
                         // to the deadline and fires it
  REQUIRE(fut.ready());
  REQUIRE(fut.ready_with_failure());
  REQUIRE_THROWS_AS(fut.get(), est::operation_timed_out);

  // The operation timed out from the caller's point of view, but nothing
  // eagerly freed it (with_timeout()'s own documented limitation, matching
  // with_stop()'s) - it's still perfectly valid to complete the promise
  // driving it. That completion must reach the already-`done`-guarded
  // first racer and be silently dropped, not double-complete `fut`.
  prom.set_value(7);
  loop.run_until_idle();

  REQUIRE_THROWS_AS(fut.get(), est::operation_timed_out); // unchanged
}

TEST_CASE("with_timeout(): the operation winning eagerly cancels the deadline timer instead of "
          "leaving it pending",
          "[with_timeout]") {
  using namespace std::chrono_literals;
  counting_resource resource;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [prom, operation] = est::make_promise_future<int>();

    // A deliberately huge timeout: if the deadline timer weren't actually
    // cancelled when `operation` wins, run_until_idle() below would have
    // nothing left to do except sleep all the way to this deadline -
    // fake_platform::interruptible_sleep_until() advancing `current` that far is exactly
    // what the assertion below would catch.
    auto fut = est::with_timeout(std::move(operation), 1000s);
    prom.set_value(42);
    loop.run_until_idle();

    REQUIRE(fut.ready());
    REQUIRE(fut.get() == 42);
    // The fake clock never had to advance - the timer was cancelled before
    // run_until_idle() ever needed to sleep toward its deadline.
    REQUIRE(fake.current == decltype(fake.current){});
  }
  // The cancelled sleep_resume_node (est:promise) was actually freed via
  // loop::cancel_timer()'s own abandon()-then-destroy path, not merely
  // left pending until loop teardown - combined with the clock assertion
  // above, this confirms cancellation ran for real.
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("with_timeout(): loop teardown while both racers are still pending completes the "
          "returned future instead of leaving it stuck (issue #113)",
          "[with_timeout]") {
  using namespace std::chrono_literals;
  counting_resource resource;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [prom, operation] = est::make_promise_future<int>();

    auto fut = est::with_timeout(std::move(operation), 1000s);
    REQUIRE_FALSE(fut.ready());

    // Neither racer has run yet: `operation` is still pending, and so is
    // the deadline timer. loop.drain_pending() (est:loop - exactly what
    // make_current_loop()'s own guard calls at scope exit, and ~loop()
    // calls too) abandons both without running either - this is the
    // scenario that used to leave `fut` stuck forever, back when
    // with_timeout<T>()'s own timer racer bridged through a second future
    // (issue #113): detail::with_timeout_timer_node::abandon() now
    // completes `fut` directly instead.
    loop.drain_pending();

    REQUIRE(fut.ready());
    REQUIRE(fut.ready_with_failure());
    REQUIRE_THROWS(fut.get());
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("with_timeout<void>(): normal completion forwards success", "[with_timeout][void]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [prom, operation] = est::make_promise_future<void>();

  auto fut = est::with_timeout(std::move(operation), 10s);
  prom.set_value();
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE_FALSE(fut.ready_with_failure());
}
