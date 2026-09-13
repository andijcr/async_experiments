import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Wraps the default resource, counting allocate()/deallocate() calls so
// leak tests can assert every allocation was balanced by a
// deallocation - the only practical way to catch a leaked when_all_state
// control block or completion-hook node in a unit test without a
// sanitizer.
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

} // namespace

TEST_CASE("when_all() with no futures resolves immediately", "[when_all]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto combined = est::when_all();
  REQUIRE(combined.ready());
}

TEST_CASE("when_all() resolves only once every future is ready, not before", "[when_all]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = est::when_all(first_future, second_future);
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready());

  first_promise.set_value(1);
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready()); // second still pending

  second_promise.set_value(2);
  loop.run_until_idle();
  REQUIRE(combined.ready());
}

TEST_CASE("when_all() resolves immediately when every future is already ready", "[when_all]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<std::string>();
  first_promise.set_value(1);
  second_promise.set_value("hello");

  auto combined = est::when_all(first_future, second_future);
  REQUIRE_FALSE(combined.ready()); // then()'s own continuation still defers to the loop
  loop.run_until_idle();
  REQUIRE(combined.ready());
}

TEST_CASE("when_all() counts a failed future the same as a succeeded one", "[when_all]") {
  // The core of issue #54's resolved failure semantics: when_all() itself
  // never fails and never inspects which input failed - it just waits for
  // every one of them to be done, success or failure - the caller checks
  // failed()/get() on each input future afterward.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = est::when_all(first_future, second_future);
  first_promise.set_value(1);
  second_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();

  REQUIRE(combined.ready());
  REQUIRE_FALSE(combined.failed());
  REQUIRE_FALSE(first_future.failed());
  REQUIRE(first_future.get() == 1);
  REQUIRE(second_future.failed());
  REQUIRE_THROWS_AS(second_future.get(), std::runtime_error);
}

TEST_CASE("when_all() still resolves when one input is abandoned while the loop keeps running",
          "[when_all]") {
  // Guards when_all_track()'s two-stage then() chain (est/src/when_all.cppm):
  // a hook registered directly on an input future is silently never
  // invoked if that future's own future_state is abandoned (destroyed
  // while still pending) rather than completed -
  // concrete_continuation<Fn, U>::abandon() (est:future) unconditionally
  // completes only its own downstream, never `fn_`. A real, plausible
  // shape for this: a helper that kicks off async work and returns a
  // combined future, letting its own local promise/future pair for one
  // of the inputs go out of scope once nothing local needs it any more -
  // modeled here by the IIFE below, which does exactly that for
  // first_promise/first_future while second_future is still very much
  // alive and later completes normally.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = [&] {
    auto [first_promise, first_future] = est::make_promise_future<int>();
    // first_promise and first_future are both destroyed here, without
    // either ever completing - abandoning first_future's own
    // future_state while when_all()'s own tracking chain is still
    // registered on it.
    return est::when_all(first_future, second_future);
  }();

  second_promise.set_value(2);
  loop.run_until_idle();
  REQUIRE(combined.ready());
}

TEST_CASE("when_all() works across heterogeneous types, including future<void>", "[when_all]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [int_promise, int_future] = est::make_promise_future<int>();
  auto [void_promise, void_future] = est::make_promise_future<void>();
  auto [string_promise, string_future] = est::make_promise_future<std::string>();

  auto combined = est::when_all(int_future, void_future, string_future);
  int_promise.set_value(7);
  void_promise.set_value();
  string_promise.set_value("world");
  loop.run_until_idle();

  REQUIRE(combined.ready());
  REQUIRE(int_future.get() == 7);
  REQUIRE(string_future.get() == "world");
}

TEST_CASE("when_all() does not consume the caller's futures", "[when_all]") {
  // The input futures stay owned by the caller throughout - when_all()
  // only ever registers a then() continuation on each, never moving or
  // otherwise taking ownership of any of them.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();

  auto combined = est::when_all(future);
  promise.set_value(42);
  loop.run_until_idle();

  REQUIRE(combined.ready());
  REQUIRE(future.ready());
  REQUIRE(future.get() == 42);
}

TEST_CASE("when_all() over a std::span resolves once every future in the range is ready",
          "[when_all]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();
  auto [third_promise, third_future] = est::make_promise_future<int>();
  std::vector<est::future<int>> futures;
  futures.push_back(std::move(first_future));
  futures.push_back(std::move(second_future));
  futures.push_back(std::move(third_future));

  auto combined = est::when_all(std::span<est::future<int>>(futures));
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready());

  first_promise.set_value(1);
  second_promise.set_value(2);
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready()); // third still pending

  third_promise.set_value(3);
  loop.run_until_idle();
  REQUIRE(combined.ready());
  REQUIRE(futures.at(0).get() == 1);
  REQUIRE(futures.at(1).get() == 2);
  REQUIRE(futures.at(2).get() == 3);
}

TEST_CASE("when_all() over an empty std::span resolves immediately", "[when_all]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto combined = est::when_all(std::span<est::future<int>>());
  REQUIRE(combined.ready());
}

TEST_CASE("when_all()'s shared state and completion hooks are freed, not leaked", "[when_all]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [first_promise, first_future] = est::make_promise_future<int>();
    auto [second_promise, second_future] = est::make_promise_future<int>();

    auto combined = est::when_all(first_future, second_future);
    first_promise.set_value(1);
    second_promise.set_value(2);
    loop.run_until_idle();
    REQUIRE(combined.ready());
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("when_all()'s shared state and completion hooks are freed even when abandoned",
          "[when_all]") {
  // Mirrors the existing "a registered continuation is freed even if
  // never invoked" future_tests.cpp case, for when_all()'s own
  // completion hooks: dropping the loop (and every promise/future) before
  // any input completes must not leak the when_all_state control block or
  // either hook's node.
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [first_promise, first_future] = est::make_promise_future<int>();
    auto [second_promise, second_future] = est::make_promise_future<int>();
    auto combined = est::when_all(first_future, second_future);
    // Nothing ever completes - loop, promises and futures all destroyed
    // here.
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}
