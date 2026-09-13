import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Same helper as loop_tests.cpp/shared_ptr_tests.cpp - counts
// allocate()/deallocate() calls so a test can assert every allocation was
// balanced, the only practical way to catch a leaked periodic-timer node
// chain in a unit test without a sanitizer.
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
// that same fake clock instead of actually blocking - identical in shape
// to est/tests/loop_tests.cpp's own fake_platform.
class fake_platform final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return current;
  }

  void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept override {
    current = std::max(current, deadline);
  }

  // A fixed seed: these tests assert jitter stays within its documented
  // bounds, never that any particular sequence of offsets occurs -
  // est/tests/jitter_tests.cpp already covers the distribution itself
  // against the real hosted_stdcpp backend.
  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override { return 1729; }

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

TEST_CASE("schedule_periodic() calls fn() once per period until cancelled from within",
          "[timer_periodic]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  int calls = 0;
  std::optional<est::periodic_timer_handle> handle;
  handle = est::schedule_periodic(1s, [&] {
    ++calls;
    if (calls == 3) {
      handle->cancel();
    }
  });

  // No stop() needed: cancelling from inside the callback stops the chain
  // from rescheduling at all, so the timer queue - and with it,
  // run_until_idle()'s own loop - goes idle on its own.
  loop.run_until_idle();

  REQUIRE(calls == 3);
}

TEST_CASE("schedule_periodic() is fixed-rate: a slow fn() doesn't drift later deadlines",
          "[timer_periodic]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  constexpr auto interval = 1s;
  constexpr auto slow_work = 400ms; // < interval, so it never pushes a
                                    // period past the next one entirely

  std::vector<std::chrono::steady_clock::time_point> call_times;
  int calls = 0;
  std::optional<est::periodic_timer_handle> handle;
  handle = est::schedule_periodic(interval, [&] {
    call_times.push_back(fake.current);
    fake.current += slow_work; // simulate fn() itself taking real time
    ++calls;
    if (calls == 3) {
      handle->cancel();
    }
  });

  loop.run_until_idle();
  REQUIRE(calls == 3);
  REQUIRE(call_times.size() == 3);

  // Each period's own start is exactly `interval` after the previous
  // one's - not interval + slow_work, which fixed-delay scheduling
  // (measuring the next deadline from when fn() *returned*, not from
  // when this period started) would have produced instead.
  REQUIRE(call_times.at(1) - call_times.at(0) == interval);
  REQUIRE(call_times.at(2) - call_times.at(1) == interval);
}

TEST_CASE("schedule_periodic() survives fn() throwing - the period is skipped, the chain "
          "still reschedules",
          "[timer_periodic]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  int calls = 0;
  std::optional<est::periodic_timer_handle> handle;
  handle = est::schedule_periodic(1s, [&] {
    ++calls;
    if (calls == 2) {
      throw std::runtime_error("transient failure");
    }
    if (calls == 3) {
      handle->cancel();
    }
  });

  // Must not throw out of run_until_idle() itself, and must still reach
  // the third, cancelling call - proving the second period's exception
  // was contained to that one period, not left to unwind the loop or
  // kill the chain.
  REQUIRE_NOTHROW(loop.run_until_idle());
  REQUIRE(calls == 3);
}

TEST_CASE("schedule_periodic()'s jittered delay stays within [interval - max_jitter, interval + "
          "max_jitter]",
          "[timer_periodic]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  constexpr auto interval = 1s;
  constexpr auto max_jitter = 200ms;

  auto last = fake.current;
  int calls = 0;
  std::optional<est::periodic_timer_handle> handle;
  handle = est::schedule_periodic(
      interval,
      [&] {
        const auto delta = fake.current - last;
        last = fake.current;
        REQUIRE(delta >= interval - max_jitter);
        REQUIRE(delta <= interval + max_jitter);
        ++calls;
        if (calls == 5) {
          handle->cancel();
        }
      },
      max_jitter);

  loop.run_until_idle();
  REQUIRE(calls == 5);
}

TEST_CASE("periodic_timer_handle::cancel() called from outside stops further firings",
          "[timer_periodic]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  int calls = 0;
  auto handle = est::schedule_periodic(1s, [&] {
    ++calls;
    loop.stop(); // break out after exactly one firing - a periodic chain
                 // that never self-cancels would otherwise keep
                 // run_until_idle() from ever going idle on its own
  });

  loop.run_until_idle();
  REQUIRE(calls == 1);

  // A second node is already scheduled (rescheduled during the first
  // fire(), before stop() took effect) - cancel it before it fires. It
  // still runs (already handed to the loop), but as a no-op that doesn't
  // call fn() or reschedule a third, so this second run_until_idle() goes
  // idle on its own once it does.
  handle.cancel();
  loop.run_until_idle();
  REQUIRE(calls == 1);
}

TEST_CASE("schedule_periodic()'s node chain and control block are freed, not leaked",
          "[timer_periodic]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);
  counting_resource resource;
  {
    est::loop loop(&resource);
    const auto loop_guard = est::make_current_loop(loop);

    int calls = 0;
    std::optional<est::periodic_timer_handle> handle;
    handle = est::schedule_periodic(1s, [&] {
      ++calls;
      if (calls == 4) {
        handle->cancel();
      }
    });

    loop.run_until_idle();
    REQUIRE(calls == 4);
  }
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("schedule_periodic()'s pending node is abandoned, not leaked, when the loop is "
          "destroyed first",
          "[timer_periodic]") {
  using namespace std::chrono_literals;
  fake_platform fake;
  const auto guard = est::platform::override_instance(fake);
  counting_resource resource;
  {
    est::loop loop(&resource);
    const auto loop_guard = est::make_current_loop(loop);
    auto handle = est::schedule_periodic(1s, [] {});
    // Loop destroyed with a periodic timer still pending, never fired.
  }
  REQUIRE(resource.allocations == resource.deallocations);
}
