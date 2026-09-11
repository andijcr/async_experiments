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
// make_promise_future<T>(): a continuation registered via then() is
// never invoked inline on the call stack that fulfills its
// promise any more - est::loop defers it to its own ready-queue, so a
// test that wants to observe a continuation's side effects must call
// loop.run_until_idle() first. `loop` is always declared before its
// promise/future pair (and stays in scope for as long as they do): every
// future_state built against it holds a bare loop&, so the loop must
// outlive it. Where a loop needs a specific memory_resource (the
// counting_resource-based leak tests), it's built as est::loop{&resource}
// - the same implicit polymorphic_allocator<std::byte> conversion
// est::loop's constructor takes directly.

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
  // Guards future_state::get()'s deduced return type: it must be
  // decltype(auto), not plain auto - plain auto strips references from
  // the return expression's type (the same rule as `auto x = expr;`),
  // which would silently turn the documented "non-consuming const T&"
  // into a fresh copy of T on every call instead of a reference to the
  // one stored value. Observed here via two separate unwrapped then()
  // registrations (each gets its own const int& argument straight from
  // future_state::get(), not exposed to this test file directly) rather
  // than by naming future_state itself, which is not part of the public
  // API surface this test file can reach.
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
  // Guards future_state::get()'s lvalue branch: a second continuation
  // reading the value must see the real one, not a moved-from leftover
  // from the first.
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
  // Guards future_state's destructor draining its continuation list: a
  // then() registered on a future whose promise is dropped without ever
  // completing must not leak the continuation node.
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
  // Guards complete()'s drain loop: then() catches a callback's
  // exception and routes it into that continuation's own downstream
  // future via set_exception() instead of letting it escape, so a
  // throwing continuation must not stop its siblings from running.
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);

  // Registration order doesn't matter to what this test proves (both
  // continuations run regardless of which drains first) - the
  // must-still-run one is simply registered first here, the throwing one
  // second, to read naturally alongside the assertions below.
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

// Guards then()'s dispatch order: a generic callback (here, an `auto&`
// lambda) is incidentally invocable both ways - with `future<int>&` and
// with `const int&` - since a template parameter binds to either.
// Unwrapped is checked first, so `value` below is deduced as `int`, not
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

// Guards fulfill()'s flatten branch: it forwards the inner future's
// value via std::move(inner_future).get(), feeding the move-taking
// set_value(T&&) overload rather than the copying set_value(const T&)
// one. std::unique_ptr<int> can't be copied at all, so this test would
// fail to compile if that ever regressed.
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
  // Exact count, not just "balanced": outer future_state<int>, then()'s
  // downstream future_state<int> + concrete_continuation node, inner
  // future_state<int>, and fulfill()'s detail::flatten_forwarder<int>
  // node - 5 total. A flatten path that called then() on the inner
  // future instead of registering flatten_forwarder<T> directly would
  // allocate a throwaway future_state<void> plus a
  // concrete_continuation<Fn, void> node nothing ever observes - 7
  // total, not 5.
  REQUIRE(resource.allocations == 5);
}

// est::future<T> itself is a coroutine's return type - no separate
// task<T> wrapper - via future<T>::promise_type. Every coroutine below
// is a plain lambda taking est::loop& as its first parameter;
// est::future<T>'s own doc comment on promise_type explains why that
// first parameter is required (it's what promise_type's own
// constructor/operator new pattern-match against) and why a plain
// function (a lambda's non-static call operator included) works as a
// coroutine here with no extra ceremony. None of these capture
// anything - state a
// coroutine needs crosses in as an ordinary by-value/by-reference
// parameter instead, since a capturing lambda's closure lives outside
// the coroutine frame it starts and isn't guaranteed to outlive it
// (clang-tidy's cppcoreguidelines-avoid-capturing-lambda-coroutines
// flags exactly this - real advice, followed here rather than
// suppressed, even though every capture below happens to be provably
// safe within its own test's scope). est::loop& itself is the one
// unavoidable reference parameter (required by the calling convention
// above) - NOLINT'd per declaration below, matching this codebase's
// already-accepted "a loop& is safe given its own documented lifetime
// precondition" stance (est::future_state<T>, est::mutex, ...).

TEST_CASE("a coroutine returning est::future<int> can co_return a value", "[future][coroutine]") {
  est::loop loop;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&) -> est::future<int> { co_return 42; };

  auto fut = coro(loop);
  // No co_await inside - initial_suspend() never suspends, so this runs
  // synchronously to completion, like a plain function, with no loop
  // involvement at all.
  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 42);
}

TEST_CASE("then() can be chained onto a future returned by a coroutine", "[future][coroutine]") {
  // A coroutine-produced future is a plain est::future<T> like any
  // other - a caller can't tell it apart from one built out of a then()
  // chain, so registering an ordinary then() continuation on
  // it must work exactly the same way, homogeneity this test exercises
  // directly rather than only through co_await (already covered by "a
  // coroutine can co_await another coroutine's future, chaining values"
  // above).
  est::loop loop;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&) -> est::future<int> { co_return 42; };

  auto chained = coro(loop).then([](int value) { return value + 1; });
  loop.run_until_idle();
  REQUIRE(chained.ready());
  REQUIRE(chained.get() == 43);
}

TEST_CASE("a coroutine returning est::future<void> can co_return with no value",
          "[future][coroutine][void]") {
  est::loop loop;
  bool ran = false;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&, bool& ran_ref) -> est::future<void> {
    ran_ref = true;
    co_return;
  };

  auto fut = coro(loop, ran);
  // No co_await inside - runs synchronously to completion, same reason
  // as "a coroutine returning est::future<int> can co_return a value"
  // above.
  REQUIRE(ran);
  REQUIRE(fut.ready());
  fut.get();
}

TEST_CASE("an exception thrown in a coroutine's body surfaces through get()",
          "[future][coroutine]") {
  est::loop loop;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&) -> est::future<int> {
    throw std::runtime_error("boom");
    co_return 0; // unreachable - co_return is only here to make this body a coroutine
  };

  // No co_await inside - runs synchronously, so unhandled_exception()
  // already ran by the time coro() returns.
  auto fut = coro(loop);
  REQUIRE(fut.ready());
  REQUIRE(fut.failed());
  REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("a coroutine can co_await another coroutine's future, chaining values",
          "[future][coroutine]") {
  est::loop loop;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto inner = [](est::loop&, int x) -> est::future<int> { co_return x * 2; };
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto outer = [](est::loop& loop_ref, decltype(inner)& inner_coro) -> est::future<int> {
    const int value = co_await inner_coro(loop_ref, 21);
    co_return value + 1;
  };

  // Both coroutines run synchronously to completion here: `inner`'s
  // co_return never suspends, so the future outer's co_await sees is
  // already ready by the time it evaluates it, and skips suspension too.
  auto fut = outer(loop, inner);
  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 43);
}

TEST_CASE("a coroutine can co_await a future built from a then() chain, genuinely suspending",
          "[future][coroutine]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto chained = future.then([](int v) { return v + 1; });
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&, est::future<int> fut) -> est::future<int> {
    const int value = co_await std::move(fut);
    co_return value * 10;
  };

  auto fut = coro(loop, std::move(chained));
  loop.run_until_idle(); // coro starts, suspends waiting on its future (not ready yet)
  REQUIRE_FALSE(fut.ready());

  promise.set_value(4);
  loop.run_until_idle(); // then()'s callback computes 5, resumes coro, coro finishes with 50

  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 50);
}

TEST_CASE("an exception in the awaited future propagates across co_await", "[future][coroutine]") {
  est::loop loop;
  auto [promise, awaited] = est::make_promise_future<int>(loop);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&, est::future<int> fut) -> est::future<int> {
    const int value = co_await std::move(fut); // rethrows once `fut` fails
    co_return value;
  };

  auto fut = coro(loop, std::move(awaited));
  loop.run_until_idle(); // coro starts, suspends waiting on its future

  promise.set_exception(std::make_exception_ptr(std::runtime_error("nope")));
  loop.run_until_idle(); // resumes into the rethrow, uncaught -> unhandled_exception()

  REQUIRE(fut.ready());
  REQUIRE(fut.failed());
  REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("a coroutine's frame and resume nodes are all freed through the loop's allocator, "
          "no leak",
          "[future][coroutine]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    auto [promise, awaited] = est::make_promise_future<int>(loop);
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto coro = [](est::loop&, est::future<int> fut) -> est::future<int> {
      const int value = co_await std::move(fut);
      co_return value + 1;
    };

    auto result = coro(loop, std::move(awaited));
    loop.run_until_idle(); // coro's frame is allocated, then suspends on `fut`
    promise.set_value(9);
    loop.run_until_idle(); // resumes, completes - frame and resume nodes freed

    REQUIRE(result.get() == 10);
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("co_await on an already-ready future resumes inline, no loop round-trip needed",
          "[future][coroutine]") {
  // future_awaiter<T>::await_ready() returns future_.ready() directly -
  // unlike then(), which always defers even for an already-ready
  // registration (see "then() registered on an already-ready future
  // still defers to the loop" above), co_await on an already-ready
  // future skips suspension entirely: the rest of the awaiting
  // coroutine's body runs immediately, right there on whatever call
  // stack reached this co_await, the same "don't wait for something
  // that isn't being waited for" stance promise_type::initial_suspend()
  // takes at the other end of a coroutine's lifetime.
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  promise.set_value(5);

  bool resumed = false;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&, est::future<int> fut, bool& resumed_ref) -> est::future<int> {
    const int value = co_await std::move(fut); // already ready - resumes inline
    resumed_ref = true;
    co_return value;
  };

  auto fut = coro(loop, std::move(future), resumed);
  REQUIRE(resumed); // no loop round-trip needed - already true by the time coro() returns
  REQUIRE(fut.get() == 5);
}

TEST_CASE("dropping an awaited future_state destroys the still-suspended coroutine, no leak",
          "[future][coroutine]") {
  // Guards future_resume_node<T>::destroy(): if the future_state a
  // coroutine is suspended awaiting is dropped without ever completing,
  // the coroutine's frame must still be destroyed, not leaked.
  //
  // The coroutine takes its future by reference, not by value, so this
  // test controls that future_state's lifetime independently of the
  // coroutine's own frame - std::optional::reset() below drops the only
  // two owning handles (promise and future) while the coroutine is still
  // suspended awaiting it.
  counting_resource resource;
  {
    est::loop loop{&resource};
    auto pair = std::optional(est::make_promise_future<int>(loop));

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto coro = [](est::loop&, est::future<int>& fut) -> est::future<int> {
      const int value = co_await fut;
      co_return value;
    };

    auto result = coro(loop, pair->second);
    loop.run_until_idle(); // coro starts, suspends awaiting pair->second by reference

    pair.reset(); // drops promise and future - future_state is destroyed
                  // while the coroutine is still suspended awaiting it
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

// A coroutine's own promise_type doesn't require est::loop& as its
// first parameter - it falls back to est::current_loop()
// (est:util.current_loop) instead, matched via the same "promise
// constructor arguments" rule that the loop-taking convention already
// relies on (see promise_type's own doc comment). Both overload shapes
// (some-parameters-but-not-loop, and no-parameters-at-all) get
// their own test - the interesting risk here isn't behavior, it's
// overload resolution: promise_type has three constructor/operator new
// pairs now, and the wrong one silently winning would either fail to
// compile (caught immediately) or, worse, compile and quietly ignore the
// intended loop.

TEST_CASE("a coroutine with no loop& parameter uses est::current_loop()", "[future][coroutine]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  auto coro = [](int value) -> est::future<int> { co_return value + 1; };

  auto fut = coro(41);
  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 42);
}

TEST_CASE("a coroutine with no parameters at all uses est::current_loop()", "[future][coroutine]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  auto coro = []() -> est::future<int> { co_return 42; };

  auto fut = coro();
  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 42);
}

TEST_CASE("a loop-less coroutine genuinely suspends and resumes via est::current_loop()",
          "[future][coroutine]") {
  // Not just "runs synchronously to completion" (both tests above never
  // hit a real suspension point) - this one actually suspends on a
  // not-yet-ready future and needs loop.run_until_idle() to resume it,
  // proving the frame was allocated against the *same* loop
  // current_loop() names, not some other one.
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::future<int>& fut) -> est::future<int> {
    const int value = co_await fut;
    co_return value * 2;
  };

  auto result = coro(future);
  REQUIRE_FALSE(result.ready());

  promise.set_value(21);
  loop.run_until_idle();
  REQUIRE(result.ready());
  REQUIRE(result.get() == 42);
}

TEST_CASE("clone() aliases the same future_state: both see the same result", "[future][clone]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto other = future.clone();
  REQUIRE_FALSE(future.ready());
  REQUIRE_FALSE(other.ready());

  promise.set_value(42);
  REQUIRE(future.ready());
  REQUIRE(other.ready());
  REQUIRE(future.get() == 42); // lvalue get(): copying, safe to also read `other` after
  REQUIRE(other.get() == 42);
}

TEST_CASE("clone() taken before the future is ready still observes a later set_value()",
          "[future][clone]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto other = future.clone();
  promise.set_value(7);
  REQUIRE(other.get() == 7);
}

TEST_CASE("both a future and its clone can register independent then() callbacks",
          "[future][clone]") {
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto other = future.clone();
  int seen_by_first = 0;
  int seen_by_second = 0;
  auto first_chain = future.then([&](int value) { seen_by_first = value; });
  auto second_chain = other.then([&](int value) { seen_by_second = value; });

  promise.set_value(9);
  loop.run_until_idle();
  REQUIRE(seen_by_first == 9);
  REQUIRE(seen_by_second == 9);
}

TEST_CASE("future<void>: two clones can each be co_awaited independently",
          "[future][clone][void]") {
  // The motivating case for clone(): a single future<void> handing out
  // N independent waiters, each safely
  // co_await-able on its own clone - nothing to consume for T=void, so
  // there's no moved-from-leftovers hazard the way there would be for a
  // value-carrying future<T> (see future<T>::clone()'s own doc comment).
  est::loop loop;
  auto [promise, future] = est::make_promise_future<void>(loop);
  auto clone_a = future.clone();
  auto clone_b = future.clone();

  bool a_resumed = false;
  bool b_resumed = false;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto waiter = [](est::future<void>& fut, bool& resumed) -> est::future<void> {
    co_await fut;
    resumed = true;
  };

  auto result_a = waiter(clone_a, a_resumed);
  auto result_b = waiter(clone_b, b_resumed);
  REQUIRE_FALSE(a_resumed);
  REQUIRE_FALSE(b_resumed);

  promise.set_value();
  loop.run_until_idle();
  REQUIRE(a_resumed);
  REQUIRE(b_resumed);
  REQUIRE(result_a.ready());
  REQUIRE(result_b.ready());
}

TEST_CASE("future<int>: two clones can each be co_awaited independently, both see the real value",
          "[future][clone]") {
  // clone() is constrained to T = void or scalar T specifically so this
  // is safe: co_await always takes future<T>::get()'s consuming (rvalue)
  // path, but "consuming" a scalar is defined to do exactly what copying
  // it would - the moved-from int is left completely unchanged - so
  // whichever clone resumes second still reads the real value, not
  // moved-from leftovers the way it would for a non-scalar T.
  est::loop loop;
  auto [promise, future] = est::make_promise_future<int>(loop);
  auto clone_a = future.clone();
  auto clone_b = future.clone();

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto waiter = [](est::future<int>& fut) -> est::future<int> { co_return co_await fut; };

  auto result_a = waiter(clone_a);
  auto result_b = waiter(clone_b);
  REQUIRE_FALSE(result_a.ready());
  REQUIRE_FALSE(result_b.ready());

  promise.set_value(7);
  loop.run_until_idle();
  REQUIRE(result_a.ready());
  REQUIRE(result_b.ready());
  REQUIRE(result_a.get() == 7);
  REQUIRE(result_b.get() == 7);
}
