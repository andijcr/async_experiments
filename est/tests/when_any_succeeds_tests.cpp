import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Wraps the default resource, counting allocate()/deallocate() calls so
// leak tests can assert every allocation was balanced by a
// deallocation - the only practical way to catch a leaked shared state
// or completion-hook node in a unit test without a sanitizer.
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

TEST_CASE("when_any_succeeds() resolves true as soon as one input succeeds",
          "[when_any_succeeds]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = est::when_any_succeeds(first_future, second_future);
  first_promise.set_value(1);
  loop.run_until_idle();

  REQUIRE(combined.ready());
  REQUIRE(combined.get());
  // second_future is left deliberately unset - a success elsewhere is
  // already enough.
}

TEST_CASE("when_any_succeeds() ignores a later completion once it has already resolved",
          "[when_any_succeeds]") {
  // Guards when_any_succeeds_state's own `done` flag: once the first
  // success has already completed `result`, a later input finishing -
  // whichever way - must not touch `result` again (set_value() on an
  // already-completed future_state is a checked precondition violation
  // elsewhere in this codebase). Exercises both a later success and a
  // later failure arriving after the winner already resolved things.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();
  auto [third_promise, third_future] = est::make_promise_future<int>();

  auto combined = est::when_any_succeeds(first_future, second_future, third_future);
  first_promise.set_value(1);
  loop.run_until_idle();
  REQUIRE(combined.ready());
  REQUIRE(combined.get());

  second_promise.set_value(2); // a later success - must be a no-op
  third_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom"))); // ditto
  loop.run_until_idle();

  REQUIRE(combined.ready());
  REQUIRE(combined.get()); // unchanged
}

TEST_CASE("when_any_succeeds() does not resolve while one input has failed but another is "
          "still pending",
          "[when_any_succeeds]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = est::when_any_succeeds(first_future, second_future);
  first_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();

  REQUIRE_FALSE(combined.ready()); // second_future could still succeed
}

TEST_CASE("when_any_succeeds() resolves false once every input has failed", "[when_any_succeeds]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = est::when_any_succeeds(first_future, second_future);
  first_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready());

  second_promise.set_exception(std::make_exception_ptr(std::runtime_error("bang")));
  loop.run_until_idle();
  REQUIRE(combined.ready());
  REQUIRE_FALSE(combined.get());
}

TEST_CASE("when_any_succeeds() resolves true even when it wins after some inputs already failed",
          "[when_any_succeeds]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = est::when_any_succeeds(first_future, second_future);
  first_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready());

  second_promise.set_value(2);
  loop.run_until_idle();
  REQUIRE(combined.ready());
  REQUIRE(combined.get());
}

TEST_CASE("when_any_succeeds() resolves synchronously when one input already succeeded",
          "[when_any_succeeds]") {
  // Guards the same "then_fast() at both stages" fast path
  // est::when_all()/est::when_any() rely on: an already-ready input
  // resolves its whole two-stage chain inline, with no loop round trip.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();
  first_promise.set_value(1);

  auto combined = est::when_any_succeeds(first_future, second_future);
  REQUIRE(combined.ready());
  REQUIRE(combined.get());
}

TEST_CASE("when_any_succeeds() with an empty pack resolves false immediately",
          "[when_any_succeeds]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto combined = est::when_any_succeeds();
  REQUIRE(combined.ready());
  REQUIRE_FALSE(combined.get());
}

TEST_CASE("when_any_succeeds() treats an abandoned input the same as a failed one, not a success",
          "[when_any_succeeds]") {
  // Mirrors est::when_all()'s/est::when_any()'s own abandonment
  // regression tests: a helper that kicks off async work and returns a
  // when_any_succeeds()-built future, letting its own local
  // promise/future pair for one of the inputs go out of scope once
  // nothing local needs it any more. Without when_any_succeeds_track()'s
  // two-stage then_fast() chain, a hook registered directly on that
  // input would never run at all - abandon() only completes its own
  // (discarded) downstream, never the hook itself.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = [&] {
    auto [first_promise, first_future] = est::make_promise_future<int>();
    // first_promise and first_future are both destroyed here, without
    // either ever completing - abandoning first_future's own
    // future_state while when_any_succeeds()'s own tracking chain is
    // still registered on it.
    return est::when_any_succeeds(first_future, second_future);
  }();
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready()); // abandonment alone isn't a success

  second_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();
  REQUIRE(combined.ready());
  REQUIRE_FALSE(combined.get()); // both accounted for, neither succeeded
}

TEST_CASE("when_any_succeeds() does not consume the caller's futures", "[when_any_succeeds]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();

  auto combined = est::when_any_succeeds(future);
  promise.set_value(42);
  loop.run_until_idle();

  REQUIRE(combined.ready());
  REQUIRE(combined.get());
  REQUIRE(future.ready());
  REQUIRE(future.get() == 42);
}

TEST_CASE("when_any_succeeds() over a std::span resolves true once any future in the range "
          "succeeds",
          "[when_any_succeeds]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();
  std::vector<est::future<int>> futures;
  futures.push_back(std::move(first_future));
  futures.push_back(std::move(second_future));

  auto combined = est::when_any_succeeds(std::span<est::future<int>>(futures));
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready());

  second_promise.set_value(2);
  loop.run_until_idle();
  REQUIRE(combined.ready());
  REQUIRE(combined.get());
  REQUIRE(futures.at(1).get() == 2);
}

TEST_CASE("when_any_succeeds() over an empty std::span resolves false immediately",
          "[when_any_succeeds]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto combined = est::when_any_succeeds(std::span<est::future<int>>());
  REQUIRE(combined.ready());
  REQUIRE_FALSE(combined.get());
}

TEST_CASE("when_any_succeeds()'s shared state and completion hooks are freed, not leaked",
          "[when_any_succeeds]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [first_promise, first_future] = est::make_promise_future<int>();
    auto [second_promise, second_future] = est::make_promise_future<int>();

    auto combined = est::when_any_succeeds(first_future, second_future);
    first_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
    loop.run_until_idle();
    REQUIRE_FALSE(combined.ready());

    second_promise.set_value(2);
    loop.run_until_idle();
    REQUIRE(combined.ready());
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("when_any_succeeds()'s shared state and completion hooks are freed even when abandoned",
          "[when_any_succeeds]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [first_promise, first_future] = est::make_promise_future<int>();
    auto [second_promise, second_future] = est::make_promise_future<int>();
    auto combined = est::when_any_succeeds(first_future, second_future);
    // Nothing ever completes - loop, promises and futures all destroyed
    // here.
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}
