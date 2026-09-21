import est;
import std;

#include <catch2/catch_test_macros.hpp>

// No real second thread anywhere in this file: an "external" writer is
// simulated by assigning directly to the std::atomic<T> a test itself
// constructs the bridge over - single-threaded, matching every other
// test in this codebase. est::external_event<T>'s own poll() is the only
// thing that ever reads that atomic; everything else here just checks
// the resulting est::future<void>/value()/reset() behavior, the same way
// est/tests/event_tests.cpp checks est::binary_event<Mode> directly.

TEST_CASE("external_event: poll() with no change from source is a no-op", "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source{42};
  est::external_event<int> bridge{source};

  bridge.poll();
  auto fut = bridge.wait();
  REQUIRE_FALSE(fut.ready());
}

TEST_CASE("external_event: poll() after a change resolves a queued wait()", "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source{0};
  est::external_event<int> bridge{source};

  auto fut = bridge.wait();
  REQUIRE_FALSE(fut.ready());

  source.store(1, std::memory_order_release);
  bridge.poll();
  loop.run_until_idle();
  REQUIRE(fut.ready());
  REQUIRE(bridge.value() == 1);
}

TEST_CASE("external_event: poll() after a change resolves wait() called afterward too",
          "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<bool> source{false};
  est::external_event<bool> bridge{source};

  source.store(true, std::memory_order_release);
  bridge.poll();

  // binary_event<manual>'s own already-signaled fast path - wait() called
  // after the poll() that observed the change resolves synchronously,
  // same as est/tests/event_tests.cpp's own binary_event<manual> tests.
  auto fut = bridge.wait();
  REQUIRE(fut.ready());
  REQUIRE(bridge.value());
}

TEST_CASE("external_event: multiple concurrent waiters all resolve off one poll()",
          "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source{0};
  est::external_event<int> bridge{source};

  auto first = bridge.wait();
  auto second = bridge.wait();
  REQUIRE_FALSE(first.ready());
  REQUIRE_FALSE(second.ready());

  source.store(7, std::memory_order_release);
  bridge.poll();
  loop.run_until_idle();
  REQUIRE(first.ready());
  REQUIRE(second.ready());
}

TEST_CASE("external_event: a second change before reset() is swallowed", "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source{0};
  est::external_event<int> bridge{source};

  source.store(1, std::memory_order_release);
  bridge.poll();
  REQUIRE(bridge.wait().ready());
  REQUIRE(bridge.value() == 1);

  // Changed again without an intervening reset() - value() reflects the
  // latest, but the event itself is still "signaled from before," not a
  // fresh signal for this second change (binary_event::set() past the
  // first successful call is a no-op until reset()).
  source.store(2, std::memory_order_release);
  bridge.poll();
  REQUIRE(bridge.value() == 2);
  REQUIRE(bridge.wait().ready()); // still resolves - just not a *new* signal
}

TEST_CASE("external_event: reset() re-arms for the next change", "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source{0};
  est::external_event<int> bridge{source};

  source.store(1, std::memory_order_release);
  bridge.poll();
  REQUIRE(bridge.wait().ready());

  bridge.reset();
  auto fut = bridge.wait();
  REQUIRE_FALSE(fut.ready());

  source.store(2, std::memory_order_release);
  bridge.poll();
  loop.run_until_idle();
  REQUIRE(fut.ready());
  REQUIRE(bridge.value() == 2);
}

TEST_CASE("external_event: works over std::atomic<bool>, a flag-style source", "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<bool> source{false};
  est::external_event<bool> bridge{source};

  auto fut = bridge.wait();
  REQUIRE_FALSE(fut.ready());

  source.store(true, std::memory_order_release);
  bridge.poll();
  loop.run_until_idle();
  REQUIRE(fut.ready());
  REQUIRE(bridge.value());
}

TEST_CASE("external_event: bridged into a real schedule_periodic() poll loop", "[external_event]") {
  using namespace std::chrono_literals;

  class fake_platform final : public est::platform::interface {
  public:
    [[nodiscard]] auto now() const noexcept -> est::platform::clock::time_point override {
      return current;
    }
    void sleep_until(est::platform::clock::time_point deadline) const noexcept override {
      current = std::max(current, deadline);
    }
    [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override { return 7; }
    [[noreturn]] void assert_failure(std::string_view /*message*/,
                                     std::source_location /*location*/) const noexcept override {
      std::abort();
    }
    void vprintdbg(std::string_view /*fmt*/, std::format_args /*args*/) const noexcept override {}
    void reset_loop_stall_detection() noexcept override {}
    void detect_loop_stall(est::platform::clock::duration /*threshold*/) const noexcept override {}

    mutable est::platform::clock::time_point current;
  };

  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  std::atomic<int> sensor{0};
  est::external_event<int> bridge{sensor};

  int observed = -1;
  // A captureless lambda taking bridge/observed as parameters, not
  // captures - a capturing lambda coroutine is a real use-after-free
  // hazard (the closure object holding the captures is a temporary that
  // dies at the end of the immediately-invoked full-expression, while the
  // coroutine frame itself can outlive it); parameters, unlike captures,
  // are copied straight into the coroutine frame, matching
  // est/tests/event_tests.cpp's own identical pattern.
  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto consumer_fn = [](est::external_event<int>& bridge_ref,
                        int& observed_ref) -> est::future<void> {
    co_await bridge_ref.wait();
    observed_ref = bridge_ref.value();
    co_return;
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto consumer = consumer_fn(bridge, observed);

  REQUIRE_FALSE(consumer.ready());

  // The "external" write happens between periodic polls, exactly the
  // shape the issue describes: a periodic timer's own callback is the
  // only thing that ever calls poll(). std::optional, not a direct
  // `auto handle = ...` - handle.cancel() is called from inside the very
  // lambda schedule_periodic() is constructed from, so handle must exist
  // (even if not yet assigned) before that call, matching
  // est/tests/timer_periodic_tests.cpp's own identical pattern.
  std::optional<est::periodic_timer_handle> handle;
  handle = est::schedule_periodic(10ms, [&] {
    bridge.poll();
    if (bridge.value() != 0) {
      handle->cancel();
    }
  });

  sensor.store(99, std::memory_order_release);
  loop.run_until_idle();

  REQUIRE(consumer.ready());
  REQUIRE(observed == 99);
}
