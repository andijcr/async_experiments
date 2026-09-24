import est;
import std;

#include <catch2/catch_test_macros.hpp>

// No real second thread anywhere in this file: an "external" writer is
// simulated by assigning directly to the std::atomic<T> a test itself
// constructs the bridge over - single-threaded, matching every other
// test in this codebase. est::external_event<T>'s own poll() is the only
// thing that ever reads that atomic; everything else here just checks
// the resulting est::future<void>/value()/reset() behavior, the same way
// est/tests/event_tests.cpp checks est::binary_event<Mode> directly. The
// one test that does exercise a real second thread lives in
// external_event_thread_tests.cpp instead - kept separate so this file
// (pure single-call-stack logic) can also run on targets with no
// <thread> at all, such as mps2an385's no-OS-threads build.

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
    [[nodiscard]] auto uptime() const noexcept -> est::platform::clock::time_point override {
      return current;
    }
    void interruptible_sleep_until(est::platform::clock::time_point deadline) noexcept override {
      current = std::max(current, deadline);
    }
    void wake(est::platform::interface::WakeId /*id*/) noexcept override {}
    void wake_all() noexcept override {}
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

TEST_CASE("external_event: the loop-registering constructor polls without a caller-driven timer",
          "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source{0};
  est::external_event<int> bridge{source, loop};

  auto fut = bridge.wait();
  REQUIRE_FALSE(fut.ready());

  // No bridge.poll() call anywhere below - notifier().notify() is what
  // makes the loop pick this up on its own, at its own dispatch
  // checkpoint (issue #125's whole point).
  source.store(1, std::memory_order_release);
  bridge.notifier().notify();
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE(bridge.value() == 1);
}

TEST_CASE("external_event: notifier().notify() with nothing actually changed is a harmless no-op",
          "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source{42};
  est::external_event<int> bridge{source, loop};
  auto notifier = bridge.notifier();

  auto fut = bridge.wait();
  notifier.notify(); // source_ never actually wrote a new value
  loop.run_until_idle();

  REQUIRE_FALSE(fut.ready());
}

TEST_CASE("external_event: unregisters from the loop when destroyed", "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source1{0};
  std::atomic<int> source2{0};
  // Stays alive for the whole test - its own notifier() is what triggers
  // the real registry walk below.
  est::external_event<int> bridge2{source2, loop};

  {
    est::external_event<int> bridge1{source1, loop};
    (void)bridge1; // registered, then immediately destroyed below
  }

  // poll_external_if_pending() (est:loop) only walks the registered-
  // source list when the shared flag is actually set - without this,
  // bridge1's own destruction going unnoticed wouldn't be exercised at
  // all. If ~external_event() hadn't unregistered bridge1, this walk
  // would poll a dangling pointer - a real use-after-free the sanitize
  // preset (ASan) would catch, not just a logical assertion.
  source2.store(1, std::memory_order_release);
  bridge2.notifier().notify();
  loop.run_until_idle();

  REQUIRE(bridge2.value() == 1);
}

TEST_CASE("external_event: multiple loop-registered sources all get polled off one notify",
          "[external_event]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source1{0};
  std::atomic<int> source2{0};
  est::external_event<int> bridge1{source1, loop};
  est::external_event<int> bridge2{source2, loop};

  auto fut1 = bridge1.wait();
  auto fut2 = bridge2.wait();

  // Both sources change; only bridge1's own notifier() ever fires - the
  // shared flag/registry walk (not a per-source signal) is what's
  // actually responsible for bridge2 getting polled too.
  source1.store(1, std::memory_order_release);
  source2.store(2, std::memory_order_release);
  bridge1.notifier().notify();
  loop.run_until_idle();

  REQUIRE(fut1.ready());
  REQUIRE(fut2.ready());
  REQUIRE(bridge1.value() == 1);
  REQUIRE(bridge2.value() == 2);
}
