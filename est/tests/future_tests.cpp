import est;

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <utility>

namespace {

using std::pmr::memory_resource;

// Wraps the default resource, counting allocate()/deallocate() calls so
// tests can assert every allocation was balanced by a deallocation - the
// only practical way to catch a leaked (or wrongly-sized) continuation
// node in a unit test without a sanitizer.
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

  // Unqualified memory_resource (via the using-declaration above) keeps this
  // signature under the column limit with [[nodiscard]] kept, rather than
  // relying on a return-type line-wrap: clang-format 18 (local) and 22 (CI)
  // disagree on how to wrap this signature when written with the fully
  // qualified std::pmr::memory_resource name.
  [[nodiscard]] auto do_is_equal(const memory_resource& other) const noexcept -> bool override {
    return this == &other;
  }
};

} // namespace

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

  bool invoked = false;
  future.then([&](est::shared_state<int>& state) {
    invoked = true;
    REQUIRE_THROWS_AS((void)state.get(), std::runtime_error);
  });
  REQUIRE(invoked);
}

TEST_CASE("multiple then() registrations are all invoked on completion", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  int count = 0;
  future.then([&](est::shared_state<int>&) { ++count; });
  future.then([&](est::shared_state<int>&) { ++count; });

  promise.set_value(1);
  REQUIRE(count == 2);
}

TEST_CASE("every then() registration observes the same, correct value via get()", "[future]") {
  // Regression test: get() used to move the value out of shared_state on
  // its first call, so a second continuation reading it would see a
  // moved-from value instead of the real one.
  auto [promise, future] = est::make_promise_future<int>();
  int first_observed = -1;
  int second_observed = -1;
  future.then([&](est::shared_state<int>& state) { first_observed = state.get(); });
  future.then([&](est::shared_state<int>& state) { second_observed = state.get(); });

  promise.set_value(42);
  REQUIRE(first_observed == 42);
  REQUIRE(second_observed == 42);
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
  {
    auto dropped = std::move(future); // destroyed at the end of this scope
  }

  promise.set_value(1); // shared_state stays alive via the promise's own reference
  SUCCEED("no crash");
}

TEST_CASE("a registered continuation is freed even if never invoked (broken promise)", "[future]") {
  // Regression test: shared_state's destructor didn't drain its
  // continuation list, so a then() registered on a future whose promise
  // is dropped without ever completing leaked the continuation node
  // forever - it just sat, unreachable, in the mutex's waiter list.
  counting_resource resource;
  {
    auto [promise, future] = est::make_promise_future<int>(&resource);
    future.then([](est::shared_state<int>&) {});
    // promise and future both destroyed here, never completed.
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("a throwing continuation still gets its node freed, not leaked", "[future]") {
  // Regression test: run() used to deallocate through delete_object on a
  // continuation_node& (wrong size/alignment for the actual derived
  // type - separately fixed) without a try/finally-equivalent, so an
  // exception from invoke() skipped deallocation entirely and left the
  // node leaked; a still-queued second continuation was abandoned
  // (leaked) too, since complete()'s drain loop never got to it.
  counting_resource resource;
  {
    auto [promise, future] = est::make_promise_future<int>(&resource);

    // The drain order is LIFO (documented on est::mutex's waiter list),
    // so the *second*-registered continuation runs first: register the
    // one that must not run first, and the throwing one second, so it's
    // the one actually dequeued and invoked first.
    bool should_not_run = false;
    future.then([&](est::shared_state<int>&) { should_not_run = true; });
    future.then([](est::shared_state<int>&) { throw std::runtime_error("boom"); });

    REQUIRE_THROWS_AS(promise.set_value(1), std::runtime_error);
    REQUIRE_FALSE(should_not_run); // documented M2-scope limitation: drain aborts on throw
  } // promise and future destroyed here; shared_state's destructor drains
    // the still-queued continuation node too.

  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}
