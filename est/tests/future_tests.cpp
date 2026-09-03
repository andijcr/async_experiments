import est;

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <utility>

TEST_CASE("set_value then get() returns the value", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  REQUIRE_FALSE(future.ready());
  promise.set_value(42);
  REQUIRE(future.ready());
  REQUIRE(future.get() == 42);
}

TEST_CASE("set_exception then get() rethrows", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(future.ready());
  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("then() registered before set_value runs synchronously on completion", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  bool invoked = false;
  int observed = 0;
  future.then([&](est::shared_state<int>& state) {
    invoked = true;
    observed = state.get();
  });
  REQUIRE_FALSE(invoked);

  promise.set_value(7);
  REQUIRE(invoked);
  REQUIRE(observed == 7);
}

TEST_CASE("then() registered after set_value runs immediately", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(9);

  bool invoked = false;
  int observed = 0;
  future.then([&](est::shared_state<int>& state) {
    invoked = true;
    observed = state.get();
  });
  REQUIRE(invoked);
  REQUIRE(observed == 9);
}

TEST_CASE("then() observes a stored exception via get()", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool caught = false;
  future.then([&](est::shared_state<int>& state) {
    try {
      state.get();
    } catch (const std::runtime_error&) {
      caught = true;
    }
  });
  REQUIRE(caught);
}

TEST_CASE("multiple then() registrations are all invoked on completion", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  int count = 0;
  future.then([&](est::shared_state<int>&) { ++count; });
  future.then([&](est::shared_state<int>&) { ++count; });

  promise.set_value(1);
  REQUIRE(count == 2);
}

TEST_CASE("promise/future are move-only and moving transfers ownership", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  auto promise2 = std::move(promise);
  auto future2 = std::move(future);

  promise2.set_value(3);
  REQUIRE(future2.get() == 3);
}

TEST_CASE("dropping the future doesn't prevent the promise from completing", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  { auto dropped = std::move(future); } // destroyed here, releases its reference

  promise.set_value(1); // shared_state stays alive via the promise's own reference
  SUCCEED("no crash");
}
