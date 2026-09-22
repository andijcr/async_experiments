import est;
import std;

#include <catch2/catch_test_macros.hpp>

// Split out of spsc_ring_tests.cpp: the one test in that file's own
// coverage that used a genuine second OS thread rather than a
// single-threaded simulation, kept apart so mps2an385's no-OS-threads
// build (EST_NO_THREADS, cmake/toolchain-mps2an385.cmake's own comment -
// <thread> doesn't exist there) can still build and run every other
// spsc_ring_tests.cpp case, none of which need real threads at all.

TEST_CASE("spsc_ring: a real std::jthread producer and the loop-thread consumer stay correct",
          "[spsc_ring]") {
  using namespace std::chrono_literals;

  // est::spsc_ring<T> is the one type in this codebase whose whole
  // contract is a real cross-thread handoff, so it earns the one test
  // that actually crosses threads, on top of (not instead of) the
  // single-threaded coverage in spsc_ring_tests.cpp. Uses the real
  // platform::interface est/tests/test_main.cpp installs
  // (std::this_thread::sleep_until, a real clock) rather than a fake
  // one - a fake clock's sleep_until() doesn't actually block, which
  // would starve the producer thread of any real wall-clock window to
  // run in between the loop-thread's drain passes.
  constexpr int item_count = 2000;
  est::spsc_ring<int> ring(16); // small on purpose - forces real full/empty contention

  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  // try_push() takes T&& (see its own doc comment in spsc_ring.cppm), so
  // each call needs a prvalue - not `i` itself, a named loop variable and
  // therefore an lvalue. This helper materializes one by an ordinary
  // pass-by-value/return-by-value call, without the no-op cast or the
  // no-op std::move() of an already-trivially-copyable int that
  // clang-tidy flags either of as redundant.
  auto as_prvalue = [](int value) { return value; };
  std::jthread producer([&ring, as_prvalue] {
    for (int i = 0; i < item_count; ++i) {
      while (!ring.try_push(as_prvalue(i))) {
        std::this_thread::yield(); // ring momentarily full - real cross-thread backpressure
      }
    }
  });

  std::vector<int> drained;
  drained.reserve(item_count);
  std::optional<est::periodic_timer_handle> handle;
  handle = est::schedule_periodic(1ms, [&] {
    while (const auto item = ring.try_pop()) {
      drained.push_back(*item);
    }
    if (std::cmp_greater_equal(drained.size(), item_count)) {
      handle->cancel();
    }
  });

  loop.run_until_idle();
  producer.join();

  std::vector<int> expected(item_count);
  std::ranges::iota(expected, 0);
  REQUIRE(drained == expected);
}
