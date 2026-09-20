import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

// Combines platform_tests.cpp's own vprintdbg-recording stub (so a test
// can confirm spawn()'s default exception hook actually reported
// something, or confirm it didn't) with with_timeout_tests.cpp's own
// controllable clock (so a genuinely-suspending coroutine - sleep_for() -
// can be driven to completion synchronously within a single
// run_until_idle() call).
class recording_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return current;
  }

  void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept override {
    current = std::max(current, deadline);
  }

  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override { return 42; }

  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }

  void vprintdbg(std::string_view fmt, std::format_args args) const noexcept override {
    ++diagnostic_count;
    try {
      last_diagnostic = std::vformat(fmt, args);
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
  }

  void reset_loop_stall_detection() noexcept override {}
  void
  detect_loop_stall(std::chrono::steady_clock::duration /*threshold*/) const noexcept override {}

  mutable std::chrono::steady_clock::time_point current;
  mutable int diagnostic_count = 0;
  mutable std::optional<std::string> last_diagnostic;
};

auto suspending_coro(bool& completed) -> est::future<void> {
  using namespace std::chrono_literals;
  co_await est::sleep_for(10s);
  completed = true;
}

} // namespace

TEST_CASE("spawn() dispatches a future<T> that completes successfully and reclaims its tracking "
          "entry",
          "[spawn]") {
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [prom, fut] = est::make_promise_future<int>();

  est::spawn(loop, std::move(fut));
  REQUIRE(loop.spawned_count() == 1);

  prom.set_value(42);
  loop.run_until_idle();

  REQUIRE(loop.spawned_count() == 0);
  REQUIRE(fake.diagnostic_count == 0); // success - no diagnostic
}

TEST_CASE("spawn() reports an unhandled exception via the default hook", "[spawn]") {
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [prom, fut] = est::make_promise_future<int>();

  est::spawn(loop, std::move(fut));
  prom.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();

  REQUIRE(loop.spawned_count() == 0);
  REQUIRE(fake.diagnostic_count == 1);
  REQUIRE(fake.last_diagnostic.has_value());
  REQUIRE(fake.last_diagnostic->find("boom") != std::string::npos);
}

TEST_CASE("spawn()'s default hook suppresses operation_cancelled", "[spawn]") {
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [prom, fut] = est::make_promise_future<int>();

  est::spawn(loop, std::move(fut));
  prom.set_exception(std::make_exception_ptr(est::operation_cancelled()));
  loop.run_until_idle();

  REQUIRE(loop.spawned_count() == 0);
  REQUIRE(fake.diagnostic_count == 0);
}

TEST_CASE("spawn()'s default hook suppresses an already-abandoned future's exception", "[spawn]") {
  using namespace std::chrono_literals;
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [prom, operation] = est::make_promise_future<int>();

  // with_timeout()'s own timer racer completes its returned future with
  // detail::abandoned_exception (est:loop) when loop teardown reaches it
  // before either racer resolves naturally - exactly like with_timeout_
  // tests.cpp's own "loop teardown while both racers are still pending"
  // case. detail::abandoned_exception isn't nameable from outside the
  // est module (this test file included), so this is the only way to get
  // one into a future from external, import-est-only test code.
  auto timed = est::with_timeout(std::move(operation), 1000s);
  loop.drain_pending();
  REQUIRE(timed.ready_with_failure());

  // Already ready by the time spawn() registers on it - then_fast()'s own
  // inline-if-already-ready path (Continuation-Node-Mechanism.md) means
  // this runs synchronously inside spawn() itself, no run_until_idle()
  // needed.
  est::spawn(loop, std::move(timed));

  REQUIRE(loop.spawned_count() == 0);
  REQUIRE(fake.diagnostic_count == 0);
}

TEST_CASE("spawn() lets a caller install a custom exception hook that sees every exception "
          "unfiltered",
          "[spawn]") {
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  int hook_calls = 0;
  loop.set_spawn_exception_hook([&hook_calls](std::exception_ptr) { ++hook_calls; });

  auto [prom, fut] = est::make_promise_future<int>();
  est::spawn(loop, std::move(fut));
  // operation_cancelled: suppressed by the *default* hook, but a caller-
  // installed one (this test's own) gets no exemption - it decides for
  // itself what counts as routine.
  prom.set_exception(std::make_exception_ptr(est::operation_cancelled()));
  loop.run_until_idle();

  REQUIRE(hook_calls == 1);
  REQUIRE(fake.diagnostic_count == 0); // the default printer never ran
}

TEST_CASE("spawn() accepts a callable, invoking it synchronously at the call site", "[spawn]") {
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  bool invoked = false;
  est::spawn(loop, [&invoked] {
    invoked = true;
    return est::make_ready_future<int>(7);
  });

  REQUIRE(invoked); // called eagerly, not deferred to a later drain
  loop.run_until_idle();
  REQUIRE(loop.spawned_count() == 0);
}

TEST_CASE("spawn() stamps the given Priority on its own completion continuation (issue #31/#106)",
          "[spawn][priority]") {
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  std::vector<est::Priority> order;
  loop.set_spawn_exception_hook(
      [&order](std::exception_ptr) { order.push_back(est::current_priority()); });

  auto [prom_low, fut_low] = est::make_promise_future<int>();
  auto [prom_high, fut_high] = est::make_promise_future<int>();
  est::spawn(loop, std::move(fut_low), est::Priority::background);
  est::spawn(loop, std::move(fut_high), est::Priority::critical);

  // Both become ready before the loop ever drains - est::loop::eager_
  // scheduler() (the default) always drains the highest non-empty level
  // first, so the critical-priority completion must run before the
  // background one despite being registered second.
  prom_low.set_exception(std::make_exception_ptr(std::runtime_error("low")));
  prom_high.set_exception(std::make_exception_ptr(std::runtime_error("high")));
  loop.run_until_idle();

  REQUIRE(order == std::vector{est::Priority::critical, est::Priority::background});
}

TEST_CASE("spawn() defaults its Priority to current_priority() at the call site",
          "[spawn][priority]") {
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  std::optional<est::Priority> observed;
  loop.set_spawn_exception_hook(
      [&observed](std::exception_ptr) { observed = est::current_priority(); });

  auto [prom, fut] = est::make_promise_future<int>();
  {
    const auto priority_guard = est::set_priority(est::Priority::high);
    est::spawn(loop, std::move(fut)); // no explicit prio - inherits ambient current_priority()
  }
  prom.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();

  REQUIRE(observed == est::Priority::high);
}

TEST_CASE("spawn() lets an unobserved coroutine run to completion via an explicit tracking entry",
          "[spawn][coroutine]") {
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  bool completed = false;

  est::spawn(loop, [&completed] { return suspending_coro(completed); });
  REQUIRE(loop.spawned_count() == 1);
  REQUIRE_FALSE(completed);

  loop.run_until_idle(); // advances the fake clock to the sleep's deadline
  REQUIRE(completed);
  REQUIRE(loop.spawned_count() == 0);
}

TEST_CASE("spawn()'s tracking entry is reclaimed on loop teardown even if the task never "
          "completes",
          "[spawn]") {
  using namespace std::chrono_literals;
  recording_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);
  {
    est::loop loop;
    const auto loop_guard = est::make_current_loop(loop);
    auto [prom, fut] = est::make_promise_future<int>();

    est::spawn(loop, std::move(fut));
    REQUIRE(loop.spawned_count() == 1);
    // Never completed - loop teardown below (make_current_loop()'s own
    // guard calls drain_pending()) must still reclaim the tracking entry.
  }
  // No crash, no leaked spawn_entry<T> - loop::drain_pending()'s own
  // unconditional spawned_.clear() (est:loop) is what guarantees this even
  // though the completion continuation itself was abandoned rather than
  // run, and so never reached untrack_spawned() on its own.
}
