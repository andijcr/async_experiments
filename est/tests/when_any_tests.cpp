import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Wraps the default resource, counting allocate()/deallocate() calls so
// leak tests can assert every allocation was balanced by a
// deallocation - the only practical way to catch a leaked event control
// block or completion-hook node in a unit test without a sanitizer.
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

TEST_CASE("when_any() resolves once any one future is ready, not before", "[when_any]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = est::when_any(first_future, second_future);
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready());

  first_promise.set_value(1);
  loop.run_until_idle();
  REQUIRE(combined.ready());

  // second_future is left deliberately unset - when_any() doesn't need
  // it to ever complete.
}

TEST_CASE("when_any() resolves synchronously when at least one future is already ready",
          "[when_any]") {
  // Guards the same "event.wait() called last" ordering est::when_all()
  // needed (docs/wiki/Continuation-Node-Mechanism.md): calling wait()
  // before every input is registered would defeat then_fast()'s own
  // point, forcing even an already-ready input through a redundant loop
  // round trip instead of resolving inline.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();
  first_promise.set_value(1);

  auto combined = est::when_any(first_future, second_future);
  REQUIRE(combined.ready());
}

TEST_CASE("when_any() resolves on a failed future the same as a succeeded one", "[when_any]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = est::when_any(first_future, second_future);
  first_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();

  REQUIRE(combined.ready());
  REQUIRE_FALSE(combined.ready_with_failure()); // when_any() itself never fails
  REQUIRE(first_future.ready_with_failure());
  REQUIRE_THROWS_AS(first_future.get(), std::runtime_error);
}

TEST_CASE("when_any() resolves when one input is abandoned while another stays pending",
          "[when_any]") {
  // Mirrors est::when_all()'s own abandonment regression test: a helper
  // that kicks off async work and returns a when_any()-built future,
  // letting its own local promise/future pair for one of the inputs go
  // out of scope once nothing local needs it any more. Without
  // when_any_track()'s two-stage then_fast() chain, a hook registered
  // directly on that input would never run - abandon() only completes
  // its own (discarded) downstream, never the hook itself - leaving
  // when_any() hung forever even though second_future genuinely never
  // completes either.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [second_promise, second_future] = est::make_promise_future<int>();

  auto combined = [&] {
    auto [first_promise, first_future] = est::make_promise_future<int>();
    // first_promise and first_future are both destroyed here, without
    // either ever completing - abandoning first_future's own
    // future_state while when_any()'s own tracking chain is still
    // registered on it.
    return est::when_any(first_future, second_future);
  }();

  loop.run_until_idle();
  REQUIRE(combined.ready());
  // second_future is still genuinely pending - when_any() resolved via
  // first_future's own abandonment instead.
}

TEST_CASE("when_any() does not consume the caller's futures", "[when_any]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();

  auto combined = est::when_any(future);
  promise.set_value(42);
  loop.run_until_idle();

  REQUIRE(combined.ready());
  REQUIRE(future.ready());
  REQUIRE(future.get() == 42);
}

TEST_CASE("when_any() works across heterogeneous types, including future<void>", "[when_any]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [int_promise, int_future] = est::make_promise_future<int>();
  auto [void_promise, void_future] = est::make_promise_future<void>();

  auto combined = est::when_any(int_future, void_future);
  void_promise.set_value();
  loop.run_until_idle();

  REQUIRE(combined.ready());
  // int_future is left deliberately unset.
}

TEST_CASE("when_any() over a std::span resolves once any future in the range is ready",
          "[when_any]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [first_promise, first_future] = est::make_promise_future<int>();
  auto [second_promise, second_future] = est::make_promise_future<int>();
  auto [third_promise, third_future] = est::make_promise_future<int>();
  std::vector<est::future<int>> futures;
  futures.push_back(std::move(first_future));
  futures.push_back(std::move(second_future));
  futures.push_back(std::move(third_future));

  auto combined = est::when_any(std::span<est::future<int>>(futures));
  loop.run_until_idle();
  REQUIRE_FALSE(combined.ready());

  second_promise.set_value(2);
  loop.run_until_idle();
  REQUIRE(combined.ready());
  REQUIRE(futures.at(1).get() == 2);
}

TEST_CASE("when_any()'s shared state and completion hooks are freed, not leaked", "[when_any]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [first_promise, first_future] = est::make_promise_future<int>();
    auto [second_promise, second_future] = est::make_promise_future<int>();

    auto combined = est::when_any(first_future, second_future);
    first_promise.set_value(1);
    loop.run_until_idle();
    REQUIRE(combined.ready());

    second_promise.set_value(2);
    loop.run_until_idle();
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("when_any()'s shared state and completion hooks are freed even when abandoned",
          "[when_any]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [first_promise, first_future] = est::make_promise_future<int>();
    auto [second_promise, second_future] = est::make_promise_future<int>();
    auto combined = est::when_any(first_future, second_future);
    // Nothing ever completes - loop, promises and futures all destroyed
    // here.
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}
