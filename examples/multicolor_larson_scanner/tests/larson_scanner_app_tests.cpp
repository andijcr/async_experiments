import est;
import larson_scanner;
import larson_scanner_app;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

// Mirrors est/tests/spsc_ring_tests.cpp's own local fake_platform:
// sleep_until() fast-forwards a fake clock instead of actually blocking,
// so a schedule_periodic()-driven test below runs instantly and
// deterministically rather than depending on real wall-clock timing.
class fake_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return current;
  }
  void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept override {
    current = std::max(current, deadline);
  }
  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override { return 7; }
  [[noreturn]] void assert_failure(std::string_view /*message*/,
                                   std::source_location /*location*/) const noexcept override {
    std::abort();
  }
  void vprintdbg(std::string_view /*fmt*/, std::format_args /*args*/) const noexcept override {}
  void reset_loop_stall_detection() noexcept override {}
  void
  detect_loop_stall(std::chrono::steady_clock::duration /*threshold*/) const noexcept override {}

  mutable std::chrono::steady_clock::time_point current;
};

} // namespace

TEST_CASE("app: a command queued before loop() starts is applied and observed via the render "
          "callback, which stops the loop itself once it sees the effect",
          "[larson_scanner_app]") {
  using namespace std::chrono_literals;

  // A fake, instantly-fast-forwarding clock: every one of app's three
  // periodic timers shares the same 1ms interval, so this test doesn't
  // depend on which of them a tied deadline fires first - command
  // application only ever happens on the *next* drain_ready() pass after
  // whichever poll() first notices the change, so by construction the
  // render callback below is guaranteed to observe the applied value on
  // some later call regardless of that tie-break order.
  fake_platform fake;
  const auto platform_guard = est::platform::override_instance(fake);

  larson_scanner::app_config config{.width = 3,
                                    .initial_speed = 0.0F,
                                    .initial_decay = 0.0F,
                                    .tick_interval = 1ms,
                                    .render_interval = 1ms,
                                    .command_poll_interval = 1ms,
                                    .command_queue_capacity = 4};

  // app's own callback can't capture `scanner_app` before it exists, so
  // this indirection stands in for it - set immediately after
  // construction, well before loop() ever invokes the callback.
  larson_scanner::app* self = nullptr;
  int render_calls = 0;
  float observed_speed = -1.0F;

  larson_scanner::app scanner_app(config, [&](const larson_scanner::led_buffer& buffer) {
    ++render_calls;
    observed_speed = buffer.red.speed;
    if (observed_speed == 5.0F) {
      self->push_command({.kind = larson_scanner::CommandKind::quit});
    }
  });
  self = &scanner_app;

  scanner_app.push_command({.channel = larson_scanner::ChannelSelector::red,
                            .param = larson_scanner::ParamKind::speed,
                            .value = 5.0F});

  scanner_app.loop(); // returns once the callback above pushes quit and it's drained

  REQUIRE(render_calls >= 1);
  REQUIRE(observed_speed == 5.0F);
}

TEST_CASE("app: push_command() reaches the drain loop from a real std::jthread producer",
          "[larson_scanner_app]") {
  using namespace std::chrono_literals;

  // The real platform test_main.cpp installed globally, not a fake one -
  // this is the one test in this file with a genuine second OS thread on
  // the producer side of push_command(), the same reasoning
  // est/tests/spsc_ring_tests.cpp's own real-jthread test gives for doing
  // the identical thing: a fake clock's sleep_until() doesn't actually
  // block, which would starve the producer thread of any real wall-clock
  // window to run in.
  larson_scanner::app_config config{
      .width = 3, .tick_interval = 2ms, .render_interval = 2ms, .command_poll_interval = 2ms};

  float observed_speed = -1.0F;
  larson_scanner::app scanner_app(
      config, [&](const larson_scanner::led_buffer& buffer) { observed_speed = buffer.red.speed; });

  std::jthread pusher([&scanner_app] {
    scanner_app.push_command({.channel = larson_scanner::ChannelSelector::red,
                              .param = larson_scanner::ParamKind::speed,
                              .value = 9.0F});
    // A real, generous margin (10x render_interval) for the loop thread
    // to actually render at least once with the update before this
    // thread requests shutdown - not a race, just real wall-clock slack.
    std::this_thread::sleep_for(20ms);
    scanner_app.push_command({.kind = larson_scanner::CommandKind::quit});
  });

  scanner_app.loop(); // blocks until the pusher thread above pushes quit

  REQUIRE(observed_speed == 9.0F);
}
