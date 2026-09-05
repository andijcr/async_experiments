import est;
import std;

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("an unwrapped then() receives a reference into the stored value, not a copy",
          "[future]") {
  // Regression test: the underlying future_state::get()'s deduced return
  // type must be decltype(auto), not plain auto - plain auto strips
  // references from the return expression's type (the same rule as
  // `auto x = expr;`), which would silently turn the documented
  // "non-consuming const T&" into a fresh copy of T on every call
  // instead of a reference to the one stored value. Caught by a PR
  // review comment, not a test, the first time. Observed here via two
  // separate unwrapped then() registrations (each gets its own const
  // int& argument straight from future_state::get(), not exposed to
  // this test file directly) rather than by naming future_state itself,
  // which - now that then()'s wrapped mode hands out an est::future<T>
  // instead (see below) - is no longer part of the public API surface
  // this test file can reach.
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(42);

  const int* first_address = nullptr;
  const int* second_address = nullptr;
  future.then([&](const int& value) { first_address = &value; });
  future.then([&](const int& value) { second_address = &value; });

  REQUIRE(first_address != nullptr);
  REQUIRE(first_address == second_address);
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
  auto chained = future.then([&](est::future<int>& state) {
    invoked = true;
    return state.get();
  });
  REQUIRE_FALSE(invoked);

  promise.set_value(7);
  REQUIRE(invoked);
  REQUIRE(chained.get() == 7);
}

TEST_CASE("then() registered after set_value runs immediately", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(9);

  bool invoked = false;
  auto chained = future.then([&](est::future<int>& state) {
    invoked = true;
    return state.get();
  });
  REQUIRE(invoked);
  REQUIRE(chained.get() == 9);
}

TEST_CASE("then() observes a stored exception via get()", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool invoked = false;
  auto chained = future.then([&](est::future<int>& state) {
    invoked = true;
    REQUIRE_THROWS_AS((void)state.get(), std::runtime_error);
    return -1;
  });
  REQUIRE(invoked);
  REQUIRE(chained.get() == -1);
}

TEST_CASE("multiple then() registrations are all invoked on completion", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  int count = 0;
  auto first = future.then([&](est::future<int>&) { return ++count; });
  auto second = future.then([&](est::future<int>&) { return ++count; });

  promise.set_value(1);
  REQUIRE(count == 2);
  REQUIRE(first.ready());
  REQUIRE(second.ready());
}

TEST_CASE("every then() registration observes the same, correct value via get()", "[future]") {
  // Regression test: get() used to move the value out of future_state on
  // its first call, so a second continuation reading it would see a
  // moved-from value instead of the real one.
  auto [promise, future] = est::make_promise_future<int>();
  auto first = future.then([](est::future<int>& state) { return state.get(); });
  auto second = future.then([](est::future<int>& state) { return state.get(); });

  promise.set_value(42);
  REQUIRE(first.get() == 42);
  REQUIRE(second.get() == 42);
}

TEST_CASE("then() returns a future that can itself be chained", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  auto chained = future.then([](est::future<int>& state) { return state.get() * 2; })
                     .then([](est::future<int>& state) { return state.get() + 1; });
  promise.set_value(10);
  REQUIRE(chained.get() == 21);
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

  promise.set_value(1); // future_state stays alive via the promise's own reference
  SUCCEED("no crash");
}

TEST_CASE("a registered continuation is freed even if never invoked (broken promise)", "[future]") {
  // Regression test: future_state's destructor didn't drain its
  // continuation list, so a then() registered on a future whose promise
  // is dropped without ever completing leaked the continuation node
  // forever - it just sat, unreachable, in the waiter list.
  counting_resource resource;
  {
    auto [promise, future] = est::make_promise_future<int>(&resource);
    future.then([](est::future<int>&) { return 0; });
    // promise and future both destroyed here, never completed.
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("a throwing continuation's exception is isolated to its own downstream future",
          "[future]") {
  // Regression test: complete()'s drain loop used to abort entirely when
  // a continuation threw, abandoning any later-queued sibling
  // continuation. then() now catches a callback's exception and routes
  // it into that continuation's own downstream future via
  // set_exception() instead of letting it escape - a throwing
  // continuation no longer stops its siblings from running.
  auto [promise, future] = est::make_promise_future<int>();

  // Drain order is LIFO (documented on est::waiter_list): the
  // second-registered continuation drains first. Register the
  // must-still-run one first (so it drains last) and the throwing one
  // second (so it drains first) - proving the throw doesn't stop the
  // sibling still queued behind it.
  bool should_still_run = false;
  auto normal_chained = future.then([&](est::future<int>& state) {
    should_still_run = true;
    return state.get();
  });
  auto throwing_chained =
      future.then([](est::future<int>&) -> int { throw std::runtime_error("boom"); });

  promise.set_value(1);

  REQUIRE(should_still_run);
  REQUIRE(normal_chained.get() == 1);
  REQUIRE_THROWS_AS(throwing_chained.get(), std::runtime_error);
}

TEST_CASE("a throwing continuation's node and downstream future are freed, not leaked",
          "[future]") {
  counting_resource resource;
  {
    auto [promise, future] = est::make_promise_future<int>(&resource);
    auto chained = future.then([](est::future<int>&) -> int { throw std::runtime_error("boom"); });
    promise.set_value(1);
    REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

// --- Issue #23: redesigned chaining - failed(), unwrapped-vs-wrapped
// then(), future<void>, and monadic flattening. ---

TEST_CASE("failed() is false on success and true once set_exception() runs", "[future]") {
  auto [value_promise, value_future] = est::make_promise_future<int>();
  REQUIRE_FALSE(value_future.failed());
  value_promise.set_value(1);
  REQUIRE_FALSE(value_future.failed());

  auto [error_promise, error_future] = est::make_promise_future<int>();
  error_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(error_future.failed());
}

TEST_CASE("then() with a plain-value callback (unwrapped) runs with the parent's value",
          "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(21);
  auto chained = future.then([](int value) { return value * 2; });
  REQUIRE(chained.get() == 42);
}

TEST_CASE(
    "then() with a plain-value callback (unwrapped) is skipped and auto-propagates on failure",
    "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool invoked = false;
  auto chained = future.then([&](int value) {
    invoked = true;
    return value;
  });

  REQUIRE_FALSE(invoked);
  REQUIRE(chained.failed());
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("a wrapped (future<T>&) then() callback can inspect failed() instead of catching",
          "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool saw_failure = false;
  auto chained = future.then([&](est::future<int>& state) {
    saw_failure = state.failed();
    return -1;
  });

  REQUIRE(saw_failure);
  REQUIRE(chained.get() == -1);
}

TEST_CASE("future<void>: set_value()/get() round-trip with nothing to carry", "[future][void]") {
  auto [promise, future] = est::make_promise_future<void>();
  REQUIRE_FALSE(future.ready());
  promise.set_value();
  REQUIRE(future.ready());
  REQUIRE_FALSE(future.failed());
  future.get(); // must not throw
  SUCCEED("get() returned without throwing");
}

TEST_CASE("future<void>: set_exception()/get() rethrows", "[future][void]") {
  auto [promise, future] = est::make_promise_future<void>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(future.failed());
  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("future<void>: an unwrapped (no-argument) then() runs on success", "[future][void]") {
  auto [promise, future] = est::make_promise_future<void>();
  bool invoked = false;
  auto chained = future.then([&] {
    invoked = true;
    return 7;
  });
  promise.set_value();
  REQUIRE(invoked);
  REQUIRE(chained.get() == 7);
}

TEST_CASE("future<void>: an unwrapped (no-argument) then() is skipped and propagates on failure",
          "[future][void]") {
  auto [promise, future] = est::make_promise_future<void>();
  bool invoked = false;
  auto chained = future.then([&] {
    invoked = true;
    return 7;
  });
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE_FALSE(invoked);
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("future<void>: a wrapped then() always runs and can inspect failed()", "[future][void]") {
  auto [promise, future] = est::make_promise_future<void>();
  bool saw_failure = false;
  auto chained = future.then([&](est::future<void>& state) {
    saw_failure = state.failed();
    return 0;
  });
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(saw_failure);
  REQUIRE(chained.get() == 0);
}

TEST_CASE("a void-returning then() callback produces a future<void>", "[future][void]") {
  auto [promise, future] = est::make_promise_future<int>();
  bool invoked = false;
  auto chained = future.then([&](int value) {
    invoked = true;
    (void)value;
  });
  promise.set_value(5);
  REQUIRE(invoked);
  REQUIRE(chained.ready());
  REQUIRE_FALSE(chained.failed());
  chained.get(); // void, must not throw
}

TEST_CASE("then() returning a future<U> flattens into future<U>, not future<future<U>>",
          "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  auto chained = future.then([](int value) {
    auto [inner_promise, inner_future] = est::make_promise_future<int>();
    inner_promise.set_value(value * 10);
    // NOLINTNEXTLINE(bugprone-use-after-move) - a structured binding never gets implicit
    // move-on-return
    return std::move(inner_future);
  });
  promise.set_value(4);
  REQUIRE(chained.get() == 40);
}

TEST_CASE("flattening propagates the inner future's failure into the outer future", "[future]") {
  auto [promise, future] = est::make_promise_future<int>();
  auto chained = future.then([](int value) {
    auto [inner_promise, inner_future] = est::make_promise_future<int>();
    (void)value;
    inner_promise.set_exception(std::make_exception_ptr(std::runtime_error("inner boom")));
    // NOLINTNEXTLINE(bugprone-use-after-move) - a structured binding never gets implicit
    // move-on-return
    return std::move(inner_future);
  });
  promise.set_value(1);
  REQUIRE(chained.failed());
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("then() returning future<void> flattens into future<void>", "[future][void]") {
  auto [promise, future] = est::make_promise_future<int>();
  bool inner_ran = false;
  auto chained = future.then([&](int value) {
    (void)value;
    auto [inner_promise, inner_future] = est::make_promise_future<void>();
    inner_ran = true;
    inner_promise.set_value();
    // NOLINTNEXTLINE(bugprone-use-after-move) - a structured binding never gets implicit
    // move-on-return
    return std::move(inner_future);
  });
  promise.set_value(1);
  REQUIRE(inner_ran);
  REQUIRE(chained.ready());
  chained.get();
}

TEST_CASE("flattening a chained then() frees every node involved, no leak", "[future]") {
  counting_resource resource;
  {
    auto [promise, future] = est::make_promise_future<int>(&resource);
    auto chained = future.then([&resource](int value) {
      auto [inner_promise, inner_future] = est::make_promise_future<int>(&resource);
      inner_promise.set_value(value + 1);
      // NOLINTNEXTLINE(bugprone-use-after-move) - a structured binding never gets implicit
      // move-on-return
      return std::move(inner_future);
    });
    promise.set_value(1);
    REQUIRE(chained.get() == 2);
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}
