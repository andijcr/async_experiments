import est;
import std;

#include <catch2/catch_test_macros.hpp>

// Split out of external_event_tests.cpp: the one test in that file's own
// coverage that used a genuine second OS thread rather than a
// single-threaded simulation, kept apart so mps2an385's no-OS-threads
// build (EST_NO_THREADS, cmake/toolchain-mps2an385.cmake's own comment -
// <thread> doesn't exist there) can still build and run every other
// external_event_tests.cpp case, none of which need real threads at
// all. Same split reasoning as est/tests/spsc_ring_tests.cpp/
// spsc_ring_thread_tests.cpp.

TEST_CASE("external_event: a real std::jthread producer notifies the loop via notifier()",
          "[external_event]") {
  // Mirrors est/tests/spsc_ring_thread_tests.cpp's own identical
  // reasoning: uses the real platform::interface est/tests/test_main.cpp
  // installs (a real clock, a real condition_variable-backed
  // interruptible_sleep_until()) rather than a fake one - a fake clock's
  // sleep never really blocks, so it could never prove wake() actually
  // interrupts a genuinely in-progress wait.
  using namespace std::chrono_literals;
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  std::atomic<int> source{0};
  est::external_event<int> bridge{source, loop};
  auto notifier = bridge.notifier();

  est::stop_source floor_stop;
  // A real pending timer, deliberately far past this test's own realistic
  // runtime: without something scheduled, run_impl() would see "nothing
  // ready, nothing pending" and return immediately, never actually
  // reaching interruptible_sleep_until() at all - defeating the whole
  // point of this test. Cancelled by the consumer coroutine below once
  // the external event resolves - left uncancelled, run_impl() would
  // just go right back to sleeping out its own remaining deadline once
  // the external event resolves (fire_ready_timers() only fires timers
  // actually due), so the early wake() would only shorten the *first*
  // sleep call rather than this test's overall runtime.
  auto floor = est::sleep_for(10s, floor_stop.get_token());

  int observed = -1;
  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto consumer_fn = [](est::external_event<int>& bridge_ref,
                        int& observed_ref,
                        est::stop_source& floor_stop_ref) -> est::future<void> {
    co_await bridge_ref.wait();
    observed_ref = bridge_ref.value();
    floor_stop_ref.request_stop();
    co_return;
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto consumer = consumer_fn(bridge, observed, floor_stop);

  // notifier.notify() calls platform::instance().wake() (external_event.cppm's
  // own doc comment on external_notifier explains why that one call is
  // deliberately thread-local-dispatched, unlike everything else here) -
  // platform::instance() is thread_local (platform.cppm) and nothing
  // installs a backend on a freshly spawned thread automatically, so the
  // producer has to install one on its own before calling notify(),
  // exactly like any other thread that wants to drive an est::loop does
  // (est/tests/test_main.cpp's own installation, on the main thread,
  // being the one this test's own loop thread already has). Reusing the
  // same backend object the main thread has (not a second, separate
  // hosted_stdcpp instance) is what makes this call actually reach the
  // condition_variable the main thread's interruptible_sleep_until() is
  // genuinely blocked on.
  auto& backend = est::platform::instance();
  std::jthread producer([&source, &notifier, &backend] {
    const auto producer_platform_guard = est::platform::override_instance(backend);
    std::this_thread::sleep_for(20ms);
    source.store(42, std::memory_order_release);
    notifier.notify();
  });

  const auto before = std::chrono::steady_clock::now();
  loop.run();
  const auto elapsed = std::chrono::steady_clock::now() - before;
  producer.join();

  REQUIRE(consumer.ready());
  REQUIRE(observed == 42);
  REQUIRE(floor.ready());
  REQUIRE(floor.ready_with_failure());
  // The real proof this was a genuine early wake, not the 10s floor
  // simply having elapsed on its own: comfortably under it.
  REQUIRE(elapsed < 1s);
}
