import est;
import std;

#include <catch2/catch_test_macros.hpp>

// Split out of platform_tests.cpp: the one test in that file's own
// coverage that used a genuine second OS thread rather than dispatching
// through a stub on a single call stack, kept apart so mps2an385's
// no-OS-threads build (EST_NO_THREADS, cmake/toolchain-mps2an385.cmake's
// own comment - <thread> doesn't exist there) can still build and run
// every other platform_tests.cpp case. Same split reasoning as
// est/tests/spsc_ring_tests.cpp/spsc_ring_thread_tests.cpp.

TEST_CASE("hosted_stdcpp's wake() unblocks a concurrently sleeping thread early", "[platform]") {
  // The one genuinely cross-thread test in this file: a real second
  // std::jthread blocked in interruptible_sleep_until() against a long
  // deadline, woken by this thread's own wake_all() call well before
  // that deadline would naturally pass - proves the
  // condition_variable-based implementation (hosted_stdcpp.cppm)
  // actually interrupts a real wait, not just that the two calls don't
  // crash run back-to-back on one thread.
  //
  // The sleeper thread calls interruptible_sleep_until() directly on a
  // captured `interface&`, not via est::platform::instance() - that
  // accessor is thread_local (platform.cppm's own doc comment) and
  // nothing installs a backend on a freshly spawned thread automatically;
  // a direct reference sidesteps that entirely, which is exactly what
  // this test needs (the two threads genuinely sharing one backend
  // object is the whole point, not each resolving their own).
  using namespace std::chrono_literals;
  auto& backend = est::platform::instance();
  const auto deadline = backend.uptime() + 10s;

  std::jthread sleeper([&backend, deadline] { backend.interruptible_sleep_until(deadline); });
  // Give the sleeper thread a real chance to actually enter the wait
  // before waking it - a wake_all() landing before it does is still
  // correctly observed (the predicate overload's own doc comment,
  // hosted_stdcpp.cppm), so this is a best-effort head start, not a
  // correctness requirement.
  std::this_thread::sleep_for(10ms);
  backend.wake_all();
  sleeper.join();

  REQUIRE(backend.uptime() < deadline);
}
