import est;
import std;

#include <catch2/catch_test_macros.hpp>

// No real second thread anywhere in this file: an "external" producer is
// simulated by calling try_push() directly from the test body -
// single-threaded, matching est/tests/external_event_tests.cpp's own
// established convention for the identical reason (est::spsc_ring<T>'s
// own contract only requires try_push()/try_pop() never run
// concurrently with themselves, not that they run on genuinely different
// threads to be exercised correctly).
//
// Every check below compares the std::optional<T> try_pop() returns
// directly against either a value or std::nullopt (optional's own
// operator==, well-defined either way it's engaged) rather than
// dereferencing/.value()-ing it - clang-tidy's bugprone-unchecked-
// optional-access doesn't recognize a REQUIRE(has_value()) immediately
// before as a narrowing guard, and comparing directly is both simpler to
// read and sidesteps the question entirely.

TEST_CASE("spsc_ring: try_pop() on an empty ring returns nullopt", "[spsc_ring]") {
  est::spsc_ring<int> ring(4);
  REQUIRE(ring.try_pop() == std::nullopt);
}

TEST_CASE("spsc_ring: a single push/pop round-trips the value", "[spsc_ring]") {
  est::spsc_ring<int> ring(4);
  REQUIRE(ring.try_push(42));
  REQUIRE(ring.try_pop() == 42);
  REQUIRE(ring.try_pop() == std::nullopt);
}

TEST_CASE("spsc_ring: preserves FIFO order across several pushes then pops", "[spsc_ring]") {
  est::spsc_ring<int> ring(4);
  REQUIRE(ring.try_push(1));
  REQUIRE(ring.try_push(2));
  REQUIRE(ring.try_push(3));

  REQUIRE(ring.try_pop() == 1);
  REQUIRE(ring.try_pop() == 2);
  REQUIRE(ring.try_pop() == 3);
  REQUIRE(ring.try_pop() == std::nullopt);
}

TEST_CASE("spsc_ring: try_push() rejects once the ring is full, without overwriting",
          "[spsc_ring]") {
  est::spsc_ring<int> ring(2);
  REQUIRE(ring.try_push(1));
  REQUIRE(ring.try_push(2));
  REQUIRE_FALSE(ring.try_push(3)); // full - capacity() slots, not buffer_.size()

  REQUIRE(ring.try_pop() == 1);
  REQUIRE(ring.try_pop() == 2);
  REQUIRE(ring.try_pop() == std::nullopt);
}

TEST_CASE("spsc_ring: a full ring accepts again once drained", "[spsc_ring]") {
  est::spsc_ring<int> ring(2);
  REQUIRE(ring.try_push(1));
  REQUIRE(ring.try_push(2));
  REQUIRE_FALSE(ring.try_push(3));

  REQUIRE(ring.try_pop() == 1);
  REQUIRE(ring.try_push(3)); // room again after one pop
  REQUIRE(ring.try_pop() == 2);
  REQUIRE(ring.try_pop() == 3);
}

TEST_CASE("spsc_ring: interleaved push/pop past several full wraparounds stays correct",
          "[spsc_ring]") {
  est::spsc_ring<int> ring(3);
  int next_push = 0;
  int next_expected_pop = 0;

  // Push-then-pop one at a time, many more times than buffer_.size()
  // (capacity() + 1) - the classic place a mod-arithmetic bug in
  // advance() would show up, and interleaving one-at-a-time keeps the
  // ring never more than one element deep, so this also doubles as an
  // empty/full-boundary stress rather than only a wraparound one.
  for (int i = 0; i < 50; ++i) {
    REQUIRE(ring.try_push(next_push++));
    REQUIRE(ring.try_pop() == next_expected_pop++);
  }
  REQUIRE(ring.try_pop() == std::nullopt);
}

TEST_CASE("spsc_ring: capacity() reflects the constructor argument, not buffer_.size()",
          "[spsc_ring]") {
  est::spsc_ring<int> ring(5);
  REQUIRE(ring.capacity() == 5);
}

TEST_CASE("spsc_ring: works over a trivially copyable struct, not just int", "[spsc_ring]") {
  struct reading {
    int sensor_id = 0;
    double value = 0.0;

    auto operator==(const reading&) const -> bool = default;
  };
  static_assert(std::is_trivially_copyable_v<reading>);

  est::spsc_ring<reading> ring(2);
  REQUIRE(ring.try_push(reading{.sensor_id = 1, .value = 3.5}));
  REQUIRE(ring.try_pop() == reading{.sensor_id = 1, .value = 3.5});
}

TEST_CASE("spsc_ring: moves a move-only type instead of copying it", "[spsc_ring]") {
  static_assert(!std::is_copy_constructible_v<std::unique_ptr<int>>);
  static_assert(std::is_nothrow_move_constructible_v<std::unique_ptr<int>>);

  est::spsc_ring<std::unique_ptr<int>> ring(2);
  REQUIRE(ring.try_push(std::make_unique<int>(42)));

  // Narrows within the if itself (matching this file's while (const auto
  // item = ...) pattern elsewhere) rather than a separate has_value()
  // check clang-tidy's bugprone-unchecked-optional-access won't credit.
  if (const auto popped = ring.try_pop()) {
    REQUIRE(**popped == 42);
  } else {
    FAIL("try_pop() unexpectedly returned nullopt");
  }
  REQUIRE(ring.try_pop() == std::nullopt);
}

TEST_CASE("spsc_ring: a value moved into a full slot leaves it unconsumed, not moved-from",
          "[spsc_ring]") {
  est::spsc_ring<std::unique_ptr<int>> ring(1);
  REQUIRE(ring.try_push(std::make_unique<int>(1)));

  auto rejected = std::make_unique<int>(2);
  REQUIRE_FALSE(ring.try_push(std::move(rejected)));
  REQUIRE(rejected != nullptr); // try_push() only moves from `value` once it knows there's room
  REQUIRE(*rejected == 2);
}

TEST_CASE("spsc_ring: drained by a real schedule_periodic() poll loop", "[spsc_ring]") {
  using namespace std::chrono_literals;

  class fake_platform final : public est::platform::interface {
  public:
    [[nodiscard]] auto now() const noexcept -> est::platform::clock::time_point override {
      return current;
    }
    void sleep_until(est::platform::clock::time_point deadline) const noexcept override {
      current = std::max(current, deadline);
    }
    [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override { return 11; }
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

  est::spsc_ring<int> queue(8);
  std::vector<int> drained;

  // Producer side: pushed directly, simulating an external writer - the
  // items are all already queued before the loop ever runs, exactly the
  // shape of a burst of external writes arriving between two polls.
  REQUIRE(queue.try_push(10));
  REQUIRE(queue.try_push(20));
  REQUIRE(queue.try_push(30));

  int drains = 0;
  std::optional<est::periodic_timer_handle> handle;
  handle = est::schedule_periodic(10ms, [&] {
    while (const auto item = queue.try_pop()) {
      drained.push_back(*item);
    }
    if (++drains == 1) {
      handle->cancel();
    }
  });

  loop.run_until_idle();

  REQUIRE(drained == std::vector{10, 20, 30});
  REQUIRE(queue.try_pop() == std::nullopt);
}

TEST_CASE("spsc_ring: a real std::jthread producer and the loop-thread consumer stay correct",
          "[spsc_ring]") {
  using namespace std::chrono_literals;

  // The one test in this file with a genuine second OS thread on the
  // other side of try_push()/try_pop() rather than a single-threaded
  // simulation (this file's own header comment above) - est::spsc_ring<T>
  // is the one type in this codebase whose whole contract is a real
  // cross-thread handoff, so it earns the one test that actually crosses
  // threads, on top of (not instead of) the single-threaded coverage
  // above. Uses the real platform::interface est/tests/test_main.cpp
  // installs (std::this_thread::sleep_until, a real clock) rather than a
  // fake one - a fake clock's sleep_until() doesn't actually block, which
  // would starve the producer thread of any real wall-clock window to run
  // in between the loop-thread's drain passes.
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
