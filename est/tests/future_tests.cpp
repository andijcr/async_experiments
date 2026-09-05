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

// Every test below declares its own est::loop and threads it into
// make_promise_future<T>() (M3, docs/PLAN.md): a continuation registered
// via then() is never invoked inline on the call stack that fulfills its
// promise any more - est::loop defers it to its own ready-queue, so a
// test that wants to observe a continuation's side effects must call
// loop.run_until_idle() first. `loop` is always declared before its
// promise/future pair (and stays in scope for as long as they do): every
// future_state built against it holds a bare loop&, so the loop must
// outlive it. Where a loop needs a specific memory_resource (the
// counting_resource-based leak tests), it's built as est::loop{&resource}
// - the same implicit polymorphic_allocator<std::byte> conversion the
// resource pointer already supported when passed directly to
// make_promise_future() before M3.

TEST_CASE("set_value then get() returns the value", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
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
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_value(42);

  const int* first_address = nullptr;
  const int* second_address = nullptr;
  future.then([&](const int& value) { first_address = &value; });
  future.then([&](const int& value) { second_address = &value; });
  loop.run_until_idle();

  REQUIRE(first_address != nullptr);
  REQUIRE(first_address == second_address);
}

TEST_CASE("set_exception then get() rethrows", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(future.ready());
  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("then() registered before set_value runs once the loop drains", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  bool invoked = false;
  auto chained = future.then([&](est::future<int>& state) {
    invoked = true;
    return state.get();
  });
  REQUIRE_FALSE(invoked);

  promise.set_value(7);
  REQUIRE_FALSE(invoked); // still deferred - est::loop hasn't drained yet
  loop.run_until_idle();
  REQUIRE(invoked);
  REQUIRE(chained.get() == 7);
}

TEST_CASE("then() registered on an already-ready future still defers to the loop", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_value(9);

  bool invoked = false;
  auto chained = future.then([&](est::future<int>& state) {
    invoked = true;
    return state.get();
  });
  REQUIRE_FALSE(invoked); // even an already-ready then() only enqueues, never runs inline
  loop.run_until_idle();
  REQUIRE(invoked);
  REQUIRE(chained.get() == 9);
}

TEST_CASE("then() observes a stored exception via get()", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool invoked = false;
  auto chained = future.then([&](est::future<int>& state) {
    invoked = true;
    REQUIRE_THROWS_AS((void)state.get(), std::runtime_error);
    return -1;
  });
  loop.run_until_idle();
  REQUIRE(invoked);
  REQUIRE(chained.get() == -1);
}

TEST_CASE("multiple then() registrations are all invoked on completion", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  int count = 0;
  auto first = future.then([&](est::future<int>&) { return ++count; });
  auto second = future.then([&](est::future<int>&) { return ++count; });

  promise.set_value(1);
  loop.run_until_idle();
  REQUIRE(count == 2);
  REQUIRE(first.ready());
  REQUIRE(second.ready());
}

TEST_CASE("every then() registration observes the same, correct value via get()", "[future]") {
  // Regression test: get() used to move the value out of future_state on
  // its first call, so a second continuation reading it would see a
  // moved-from value instead of the real one.
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto first = future.then([](est::future<int>& state) { return state.get(); });
  auto second = future.then([](est::future<int>& state) { return state.get(); });

  promise.set_value(42);
  loop.run_until_idle();
  REQUIRE(first.get() == 42);
  REQUIRE(second.get() == 42);
}

TEST_CASE("then() returns a future that can itself be chained", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto chained = future.then([](est::future<int>& state) { return state.get() * 2; })
                     .then([](est::future<int>& state) { return state.get() + 1; });
  promise.set_value(10);
  loop.run_until_idle();
  REQUIRE(chained.get() == 21);
}

TEST_CASE("promise/future are move-only and moving transfers ownership", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto promise2 = std::move(promise);
  auto future2 = std::move(future);

  promise2.set_value(3);
  REQUIRE(future2.get() == 3);
}

TEST_CASE("dropping the future doesn't prevent the promise from completing", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
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
    est::loop loop{&resource};
    auto [promise, future] = est::make_promise_future<int>(loop);
    future.then([](est::future<int>&) { return 0; });
    // promise, future and loop all destroyed here, never completed - the
    // continuation never even reaches est::loop's own ready-queue.
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
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);

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
  loop.run_until_idle();

  REQUIRE(should_still_run);
  REQUIRE(normal_chained.get() == 1);
  REQUIRE_THROWS_AS(throwing_chained.get(), std::runtime_error);
}

TEST_CASE("a throwing continuation's node and downstream future are freed, not leaked",
          "[future]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    auto [promise, future] = est::make_promise_future<int>(loop);
    auto chained = future.then([](est::future<int>&) -> int { throw std::runtime_error("boom"); });
    promise.set_value(1);
    loop.run_until_idle();
    REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

// --- Issue #23: redesigned chaining - failed(), unwrapped-vs-wrapped
// then(), future<void>, and monadic flattening. ---

TEST_CASE("failed() is false on success and true once set_exception() runs", "[future]") {
  est::loop loop;
  auto [value_promise, value_future] = est::make_promise_future<int>(loop);
  REQUIRE_FALSE(value_future.failed());
  value_promise.set_value(1);
  REQUIRE_FALSE(value_future.failed());

  auto [error_promise, error_future] = est::make_promise_future<int>(loop);
  error_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(error_future.failed());
}

TEST_CASE("then() with a plain-value callback (unwrapped) runs with the parent's value",
          "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_value(21);
  auto chained = future.then([](int value) { return value * 2; });
  loop.run_until_idle();
  REQUIRE(chained.get() == 42);
}

TEST_CASE(
    "then() with a plain-value callback (unwrapped) is skipped and auto-propagates on failure",
    "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool invoked = false;
  auto chained = future.then([&](int value) {
    invoked = true;
    return value;
  });
  loop.run_until_idle();

  REQUIRE_FALSE(invoked);
  REQUIRE(chained.failed());
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("a wrapped (future<T>&) then() callback can inspect failed() instead of catching",
          "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool saw_failure = false;
  auto chained = future.then([&](est::future<int>& state) {
    saw_failure = state.failed();
    return -1;
  });
  loop.run_until_idle();

  REQUIRE(saw_failure);
  REQUIRE(chained.get() == -1);
}

// Regression test for issue #39: a generic callback (here, an `auto&`
// lambda) is incidentally invocable both ways - with `future<int>&` and
// with `const int&` - since a template parameter binds to either. An
// earlier version of then() checked the wrapped shape first, so a
// generic lambda like this one silently got wrapped (no-unwrap)
// behavior even though nothing about it opted into that explicitly. Now
// unwrapped is checked first: `value` below is deduced as `int`, not
// `est::future<int>`, and this callback is skipped (not invoked) on
// failure rather than being invoked to inspect it.
TEST_CASE("a generic callback defaults to unwrapped, not wrapped, when both are viable",
          "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_value(21);

  auto chained = future.then([](auto& value) { return value * 2; });
  loop.run_until_idle();
  REQUIRE(chained.get() == 42);
}

TEST_CASE("a generic callback, defaulted to unwrapped, is skipped and auto-propagates on failure",
          "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool invoked = false;
  auto chained = future.then([&](auto& value) {
    invoked = true;
    return value;
  });
  loop.run_until_idle();

  REQUIRE_FALSE(invoked);
  REQUIRE(chained.failed());
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("future<void>: set_value()/get() round-trip with nothing to carry", "[future][void]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<void>(loop);
  REQUIRE_FALSE(future.ready());
  promise.set_value();
  REQUIRE(future.ready());
  REQUIRE_FALSE(future.failed());
  future.get(); // must not throw
  SUCCEED("get() returned without throwing");
}

TEST_CASE("future<void>: set_exception()/get() rethrows", "[future][void]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<void>(loop);
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(future.failed());
  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("future<void>: an unwrapped (no-argument) then() runs on success", "[future][void]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<void>(loop);
  bool invoked = false;
  auto chained = future.then([&] {
    invoked = true;
    return 7;
  });
  promise.set_value();
  loop.run_until_idle();
  REQUIRE(invoked);
  REQUIRE(chained.get() == 7);
}

TEST_CASE("future<void>: an unwrapped (no-argument) then() is skipped and propagates on failure",
          "[future][void]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<void>(loop);
  bool invoked = false;
  auto chained = future.then([&] {
    invoked = true;
    return 7;
  });
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();
  REQUIRE_FALSE(invoked);
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("future<void>: a wrapped then() always runs and can inspect failed()", "[future][void]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<void>(loop);
  bool saw_failure = false;
  auto chained = future.then([&](est::future<void>& state) {
    saw_failure = state.failed();
    return 0;
  });
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();
  REQUIRE(saw_failure);
  REQUIRE(chained.get() == 0);
}

TEST_CASE("a void-returning then() callback produces a future<void>", "[future][void]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  bool invoked = false;
  auto chained = future.then([&](int value) {
    invoked = true;
    (void)value;
  });
  promise.set_value(5);
  loop.run_until_idle();
  REQUIRE(invoked);
  REQUIRE(chained.ready());
  REQUIRE_FALSE(chained.failed());
  chained.get(); // void, must not throw
}

TEST_CASE("then() returning a future<U> flattens into future<U>, not future<future<U>>",
          "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto chained = future.then([&loop](int value) {
    auto [inner_promise, inner_future] = est::make_promise_future<int>(loop);
    inner_promise.set_value(value * 10);
    // NOLINTNEXTLINE(bugprone-use-after-move) - a structured binding never gets implicit
    // move-on-return
    return std::move(inner_future);
  });
  promise.set_value(4);
  loop.run_until_idle();
  REQUIRE(chained.get() == 40);
}

// Regression test for issue #25: fulfill()'s flatten branch used to
// forward the inner future's value with a plain (copying) get() call,
// which didn't just cost an extra copy - it made flattening a
// move-only-valued future<T> outright fail to compile, since
// forwarding a move-only value through the copying set_value(const T&)
// overload requires copy-constructing T. std::unique_ptr<int> can't be
// copied at all, so this test would not have compiled before the fix
// (std::move(inner_future).get() now feeds the move-taking
// set_value(T&&) overload instead).
TEST_CASE("flattening a then() that returns a future<unique_ptr<T>> moves, not copies, the value",
          "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto chained = future.then([&loop](int value) {
    auto [inner_promise, inner_future] = est::make_promise_future<std::unique_ptr<int>>(loop);
    inner_promise.set_value(std::make_unique<int>(value + 1));
    // NOLINTNEXTLINE(bugprone-use-after-move) - a structured binding never gets implicit
    // move-on-return
    return std::move(inner_future);
  });
  promise.set_value(1);
  loop.run_until_idle();
  // False positive below: chained is used exactly once, moved directly
  // into the .get() call it's cast for - clang-tidy flags the .get()
  // itself as a "use after move" rather than recognizing it as the one
  // and only use.
  // NOLINTNEXTLINE(bugprone-use-after-move)
  REQUIRE(*std::move(chained).get() == 2);
}

TEST_CASE("flattening propagates the inner future's failure into the outer future", "[future]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto chained = future.then([&loop](int value) {
    auto [inner_promise, inner_future] = est::make_promise_future<int>(loop);
    (void)value;
    inner_promise.set_exception(std::make_exception_ptr(std::runtime_error("inner boom")));
    // NOLINTNEXTLINE(bugprone-use-after-move) - a structured binding never gets implicit
    // move-on-return
    return std::move(inner_future);
  });
  promise.set_value(1);
  loop.run_until_idle();
  REQUIRE(chained.failed());
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("then() returning future<void> flattens into future<void>", "[future][void]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  bool inner_ran = false;
  auto chained = future.then([&](int value) {
    (void)value;
    auto [inner_promise, inner_future] = est::make_promise_future<void>(loop);
    inner_ran = true;
    inner_promise.set_value();
    // NOLINTNEXTLINE(bugprone-use-after-move) - a structured binding never gets implicit
    // move-on-return
    return std::move(inner_future);
  });
  promise.set_value(1);
  loop.run_until_idle();
  REQUIRE(inner_ran);
  REQUIRE(chained.ready());
  chained.get();
}

TEST_CASE("flattening a chained then() frees every node involved, no leak", "[future]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    auto [promise, future] = est::make_promise_future<int>(loop);
    auto chained = future.then([&loop](int value) {
      auto [inner_promise, inner_future] = est::make_promise_future<int>(loop);
      inner_promise.set_value(value + 1);
      // NOLINTNEXTLINE(bugprone-use-after-move) - a structured binding never gets implicit
      // move-on-return
      return std::move(inner_future);
    });
    promise.set_value(1);
    loop.run_until_idle();
    REQUIRE(chained.get() == 2);
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
  // Exact count, not just "balanced" (issue #25): outer future_state<int>,
  // then()'s downstream future_state<int> + concrete_continuation node,
  // inner future_state<int>, and fulfill()'s on_ready() raw_continuation
  // node - 5 total. Before on_ready() existed, fulfill()'s flatten path
  // called then() on the inner future purely to register its forwarding
  // callback, which allocated a throwaway future_state<void> plus a
  // concrete_continuation<Fn, void> node - 2 more, for 7 total - even
  // though nothing ever observed either one. A regression back to 7 here
  // would mean that overhead came back.
  REQUIRE(resource.allocations == 5);
}
