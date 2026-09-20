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

// Counts how many times a value was copy- vs move-constructed since the
// original (each copy/move inherits the running counts from the source
// and adds one more of its own kind) - lets a test observe which path
// concrete_continuation<Fn, U>::run()'s own `count() == 1` check (issue
// #64) actually took, by inspecting the value a by-value callback
// parameter received: the parameter binding itself is the copy or move
// being counted.
struct copy_move_tracker {
  int copies = 0;
  int moves = 0;

  copy_move_tracker() = default;
  copy_move_tracker(const copy_move_tracker& other) noexcept
      : copies(other.copies + 1), moves(other.moves) {}
  copy_move_tracker(copy_move_tracker&& other) noexcept
      : copies(other.copies), moves(other.moves + 1) {}
  auto operator=(const copy_move_tracker&) -> copy_move_tracker& = default;
  auto operator=(copy_move_tracker&&) -> copy_move_tracker& = default;
  ~copy_move_tracker() = default;
};

} // namespace

// Every test below declares its own est::loop, then registers it as
// est::current_loop() via make_current_loop() before calling
// make_promise_future<T>() (which, like every other loop-consuming
// function in this codebase, always resolves current_loop() rather than
// taking a loop& parameter): a continuation registered via then() is
// never invoked inline on the call stack that fulfills its promise -
// est::loop defers it to its own ready-queue, so a test that wants to
// observe a continuation's side effects must call loop.run_until_idle()
// first. Where a loop needs a specific memory_resource (the
// counting_resource-based leak tests), it's built as est::loop{&resource}
// - the same implicit polymorphic_allocator<std::byte> conversion
// est::loop's constructor takes directly.

TEST_CASE("set_value then get() returns the value", "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(42);

  const int* first_address = nullptr;
  const int* second_address = nullptr;
  // (void): each then()'s own downstream future<void> is never read - the
  // side effect (capturing an address) is the whole point of this test.
  (void)future.then([&](const int& value) { first_address = &value; });
  (void)future.then([&](const int& value) { second_address = &value; });
  loop.run_until_idle();

  REQUIRE(first_address != nullptr);
  REQUIRE(first_address == second_address);
}

TEST_CASE("then() copies the stored value when another handle still shares the future_state",
          "[future]") {
  // The negative case for the next two tests: `promise`/`future` both
  // stay alive through run_until_idle() below, so the future_state's
  // ref count is at least 3 (promise's own state_, future's own state_,
  // the node's owner_) by the time run() checks it - concrete_continuation
  // <Fn, U>::run()'s own `count() == 1` optimization (issue #64) must not
  // fire, and the callback's by-value parameter must be copy-, not
  // move-, constructed from the stored value.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<copy_move_tracker>();
  promise.set_value(copy_move_tracker{});

  int observed_copies = -1;
  int observed_moves = -1;
  // By-value on purpose, not an oversight: the parameter binding itself is
  // what's under test (see copy_move_tracker's own doc comment) - a
  // const& parameter would never let a move happen at all.
  // NOLINTNEXTLINE(performance-unnecessary-value-param)
  auto chained = future.then([&](copy_move_tracker value) {
    observed_copies = value.copies;
    observed_moves = value.moves;
  });
  loop.run_until_idle();

  REQUIRE(observed_copies == 1);
  REQUIRE(observed_moves == 1); // the one move already done: set_value(tracker&&) into result_
}

TEST_CASE("then() moves the stored value out when the continuation node is the sole owner "
          "of future_state",
          "[future]") {
  // Issue #64. `promise`/`future` are both confined to the immediately-
  // invoked lambda below and destroyed before run_until_idle() runs -
  // set_value() happens before then() is called, so the node already
  // holds its own owner_ shared_ptr (via set_continuation()'s
  // already-ready branch) by the time the lambda returns, leaving the
  // node as the future_state's sole owner once promise/future go out of
  // scope. count() == 1 at run() time, so the callback's by-value
  // parameter must be move-, not copy-, constructed.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  int observed_copies = -1;
  int observed_moves = -1;
  auto chained = [&] {
    auto [promise, future] = est::make_promise_future<copy_move_tracker>();
    promise.set_value(copy_move_tracker{});
    // By-value on purpose - see the first copy_move_tracker test's own
    // NOLINTNEXTLINE comment.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    return std::move(future).then([&](copy_move_tracker value) {
      observed_copies = value.copies;
      observed_moves = value.moves;
    });
  }();
  loop.run_until_idle();

  REQUIRE(observed_copies == 0);
  REQUIRE(observed_moves == 2); // set_value(tracker&&) into result_, then moved into the callback
}

TEST_CASE("std::move(future).then() releases this handle's own reference immediately", "[future]") {
  // Isolates issue #71's own contribution from issue #64's: `promise` is
  // explicitly dropped first (moved into, and destroyed alongside, a
  // nested scope) so it can't keep the future_state's ref count above 1
  // on its own - then `future` is std::move()-then()'d but deliberately
  // kept in scope (unlike the previous test) through run_until_idle().
  // Without future<T>::then(Fn&&) && releasing `future`'s own state_
  // immediately, `future` staying in scope would keep the future_state's
  // ref count at 2 for the whole loop drain, and the callback would
  // observe a copy, not a move, despite the caller having written
  // std::move(future).
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<copy_move_tracker>();
  promise.set_value(copy_move_tracker{});
  {
    auto discard = std::move(promise);
  } // NOLINT(bugprone-use-after-move)

  int observed_copies = -1;
  int observed_moves = -1;
  // By-value on purpose - see the first copy_move_tracker test's own
  // NOLINTNEXTLINE comment.
  // NOLINTNEXTLINE(performance-unnecessary-value-param)
  auto chained = std::move(future).then([&](copy_move_tracker value) {
    observed_copies = value.copies;
    observed_moves = value.moves;
  });
  loop.run_until_idle();

  REQUIRE(observed_copies == 0);
  REQUIRE(observed_moves == 2);
}

TEST_CASE("set_exception then get() rethrows", "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(future.ready());
  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("then() registered before set_value runs once the loop drains", "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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

TEST_CASE("then_fast() registered on an already-ready future runs immediately, no loop drain "
          "needed",
          "[future]") {
  // Issue #65: unlike then() above, then_fast() runs its callback right
  // here - before run_until_idle() is ever called - when the future was
  // already ready at registration time.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(9);

  bool invoked = false;
  auto chained = future.then_fast([&](est::future<int>& state) {
    invoked = true;
    return state.get();
  });
  REQUIRE(invoked);         // ran inline, synchronously, inside then_fast() itself
  REQUIRE(chained.ready()); // downstream is already complete too, no drain needed
  REQUIRE(chained.get() == 9);
}

TEST_CASE("then_fast() registered before set_value defers exactly like then()", "[future]") {
  // The not-yet-ready path is untouched by then_fast(): there's nothing
  // to run inline until set_value()/set_exception() actually completes
  // this future_state, so a not-yet-ready then_fast() enqueues into
  // waiters_ and defers through the loop precisely like then() does.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  bool invoked = false;
  auto chained = future.then_fast([&](est::future<int>& state) {
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

TEST_CASE("then_fast() propagates a stored exception exactly like then()", "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  auto chained = future.then_fast([](int value) { return value * 2; });
  REQUIRE(chained.ready()); // ran inline - auto-propagate-on-failure, fn_ not called
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("then_fast()'s inline run() defeats the sole-owner move optimization then() relies on",
          "[future]") {
  // Deliberately the opposite outcome from "then() moves the stored
  // value out when the continuation node is the sole owner of
  // future_state" (issue #64) - even reproducing that test's exact
  // shape (promise/future both confined to, and destroyed at the end
  // of, an immediately-invoked lambda) still copies here, never moves,
  // because then_fast()'s run() executes synchronously, inside
  // future<T>::then_fast(Fn&&) &&'s own call frame - its local `state`
  // (what used to be future's state_) is still alive and holding a
  // reference for the whole call, so concrete_continuation<Fn, U>::run()'s
  // own `owner_.count() == 1` check can never see just 1: at minimum,
  // that local plus owner_ itself are both alive at once. then()'s own
  // version of this test only reaches count() == 1 because run() there
  // happens *after* run_until_idle() - by which point this same lambda's
  // locals have already unwound. then_fast()'s whole point (skipping
  // that deferral) is exactly what puts this optimization out of reach.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  int observed_copies = -1;
  int observed_moves = -1;
  auto chained = [&] {
    auto [promise, future] = est::make_promise_future<copy_move_tracker>();
    promise.set_value(copy_move_tracker{});
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    return std::move(future).then_fast([&](copy_move_tracker value) {
      observed_copies = value.copies;
      observed_moves = value.moves;
    });
  }();

  REQUIRE(chained.ready()); // ran inline - no run_until_idle() needed
  REQUIRE(observed_copies == 1);
  REQUIRE(observed_moves == 1); // the one move already done: set_value(tracker&&) into result_
}

TEST_CASE("then() observes a stored exception via get()", "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  auto first = future.then([](est::future<int>& state) { return state.get(); });
  auto second = future.then([](est::future<int>& state) { return state.get(); });

  promise.set_value(42);
  loop.run_until_idle();
  REQUIRE(first.get() == 42);
  REQUIRE(second.get() == 42);
}

TEST_CASE("then() returns a future that can itself be chained", "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  auto chained = future.then([](est::future<int>& state) { return state.get() * 2; })
                     .then([](est::future<int>& state) { return state.get() + 1; });
  promise.set_value(10);
  loop.run_until_idle();
  REQUIRE(chained.get() == 21);
}

TEST_CASE("promise/future are move-only and moving transfers ownership", "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  auto promise2 = std::move(promise);
  auto future2 = std::move(future);

  promise2.set_value(3);
  REQUIRE(future2.get() == 3);
}

TEST_CASE("dropping the future doesn't prevent the promise from completing", "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  {
    auto dropped = std::move(future); // destroyed at the end of this scope
  }

  promise.set_value(1); // future_state stays alive via the promise's own reference
  SUCCEED("no crash");
}

TEST_CASE("promise::get_future() derives a future aliasing the original, before and after "
          "set_value()",
          "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, original] = est::make_promise_future<int>();

  auto derived = promise.get_future();
  REQUIRE_FALSE(derived.ready());
  REQUIRE_FALSE(original.ready());

  promise.set_value(7);
  REQUIRE(derived.ready());
  REQUIRE(derived.get() == 7);
  REQUIRE(original.ready()); // same future_state - both see the same completion
}

TEST_CASE("promise::get_future() can be called more than once, each call an independent handle "
          "onto the same future_state",
          "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, original] = est::make_promise_future<void>();

  auto first = promise.get_future();
  auto second = promise.get_future();
  promise.set_value();

  REQUIRE(first.ready());
  REQUIRE(second.ready());
  REQUIRE(original.ready());
}

TEST_CASE("a registered continuation is freed even if never invoked (broken promise)", "[future]") {
  // Guards future_state's destructor draining its continuation list: a
  // then() registered on a future whose promise is dropped without ever
  // completing must not leak the continuation node.
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [promise, future] = est::make_promise_future<int>();
    (void)future.then([](est::future<int>&) { return 0; });
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();

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
    const auto loop_guard = est::make_current_loop(loop);
    auto [promise, future] = est::make_promise_future<int>();
    auto chained = future.then([](est::future<int>&) -> int { throw std::runtime_error("boom"); });
    promise.set_value(1);
    loop.run_until_idle();
    REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

// --- Issue #23: redesigned chaining - ready_with_failure(), unwrapped-vs-wrapped
// then(), future<void>, and monadic flattening. ---

TEST_CASE("ready_with_failure() is false on success and true once set_exception() runs",
          "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [value_promise, value_future] = est::make_promise_future<int>();
  REQUIRE_FALSE(value_future.ready_with_failure());
  value_promise.set_value(1);
  REQUIRE_FALSE(value_future.ready_with_failure());

  auto [error_promise, error_future] = est::make_promise_future<int>();
  error_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(error_future.ready_with_failure());
}

TEST_CASE("ready_with_value() is false while pending or failed, true only once set_value() runs",
          "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [value_promise, value_future] = est::make_promise_future<int>();
  REQUIRE_FALSE(value_future.ready_with_value()); // still pending
  value_promise.set_value(1);
  REQUIRE(value_future.ready_with_value());
  REQUIRE_FALSE(value_future.ready_with_failure());

  auto [error_promise, error_future] = est::make_promise_future<int>();
  error_promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE_FALSE(error_future.ready_with_value()); // ready, but not with a value
}

TEST_CASE("then() with a plain-value callback (unwrapped) runs with the parent's value",
          "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(21);
  auto chained = future.then([](int value) { return value * 2; });
  loop.run_until_idle();
  REQUIRE(chained.get() == 42);
}

TEST_CASE(
    "then() with a plain-value callback (unwrapped) is skipped and auto-propagates on failure",
    "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool invoked = false;
  auto chained = future.then([&](int value) {
    invoked = true;
    return value;
  });
  loop.run_until_idle();

  REQUIRE_FALSE(invoked);
  REQUIRE(chained.ready_with_failure());
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE(
    "a wrapped (future<T>&) then() callback can inspect ready_with_failure() instead of catching",
    "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool saw_failure = false;
  auto chained = future.then([&](est::future<int>& state) {
    saw_failure = state.ready_with_failure();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_value(21);

  auto chained = future.then([](auto& value) { return value * 2; });
  loop.run_until_idle();
  REQUIRE(chained.get() == 42);
}

TEST_CASE("a generic callback, defaulted to unwrapped, is skipped and auto-propagates on failure",
          "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));

  bool invoked = false;
  auto chained = future.then([&](auto& value) {
    invoked = true;
    return value;
  });
  loop.run_until_idle();

  REQUIRE_FALSE(invoked);
  REQUIRE(chained.ready_with_failure());
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("future<void>: set_value()/get() round-trip with nothing to carry", "[future][void]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<void>();
  REQUIRE_FALSE(future.ready());
  promise.set_value();
  REQUIRE(future.ready());
  REQUIRE_FALSE(future.ready_with_failure());
  future.get(); // must not throw
  SUCCEED("get() returned without throwing");
}

TEST_CASE("future<void>: set_exception()/get() rethrows", "[future][void]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<void>();
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  REQUIRE(future.ready_with_failure());
  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("future<void>: an unwrapped (no-argument) then() runs on success", "[future][void]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<void>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<void>();
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

TEST_CASE("future<void>: a wrapped then() always runs and can inspect ready_with_failure()",
          "[future][void]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<void>();
  bool saw_failure = false;
  auto chained = future.then([&](est::future<void>& state) {
    saw_failure = state.ready_with_failure();
    return 0;
  });
  promise.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();
  REQUIRE(saw_failure);
  REQUIRE(chained.get() == 0);
}

TEST_CASE("a void-returning then() callback produces a future<void>", "[future][void]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  bool invoked = false;
  auto chained = future.then([&](int value) {
    invoked = true;
    (void)value;
  });
  promise.set_value(5);
  loop.run_until_idle();
  REQUIRE(invoked);
  REQUIRE(chained.ready());
  REQUIRE_FALSE(chained.ready_with_failure());
  chained.get(); // void, must not throw
}

TEST_CASE("then() returning a future<U> flattens into future<U>, not future<future<U>>",
          "[future]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  auto chained = future.then([](int value) {
    auto [inner_promise, inner_future] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  auto chained = future.then([](int value) {
    auto [inner_promise, inner_future] = est::make_promise_future<std::unique_ptr<int>>();
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
  const auto loop_guard = est::make_current_loop(loop);
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
  loop.run_until_idle();
  REQUIRE(chained.ready_with_failure());
  REQUIRE_THROWS_AS(chained.get(), std::runtime_error);
}

TEST_CASE("then() returning future<void> flattens into future<void>", "[future][void]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
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
  loop.run_until_idle();
  REQUIRE(inner_ran);
  REQUIRE(chained.ready());
  chained.get();
}

TEST_CASE("flattening a chained then() frees every node involved, no leak", "[future]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [promise, future] = est::make_promise_future<int>();
    auto chained = future.then([](int value) {
      auto [inner_promise, inner_future] = est::make_promise_future<int>();
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

TEST_CASE("dropping an abandoned inner future_state completes the flattened future, no leak",
          "[future]") {
  // Guards flatten_forwarder<T>::destroy(): if the inner future_state a
  // flattening then() registered a flatten_forwarder<T> on is dropped
  // without ever completing, the outer (flattened) future it forwards
  // into must still be completed (with an exception), not silently
  // left to strand a coroutine suspended awaiting it - the identical
  // hazard concrete_continuation<Fn, U>::destroy() (this file's own
  // "a throwing continuation's node and downstream future are freed"
  // test, and every resume node in this codebase) already guards
  // against, one layer over. Originally undiscovered until
  // est::mutex::lock()'s own then()-based slow path (issue #67,
  // docs/PLAN.md's "Issue #66 & #67" entry) turned this from a
  // documented-but-unexercised gap into a real, test-caught leak.
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto inner_pair = std::optional(est::make_promise_future<int>());
    auto [outer_promise, outer_future] = est::make_promise_future<int>();

    auto chained =
        outer_future.then([&inner_pair](int /*value*/) { return std::move(inner_pair->second); });
    outer_promise.set_value(1);
    loop.run_until_idle(); // then()'s callback runs, returns the still-pending inner future -
                           // fulfill() registers a flatten_forwarder<int> on its future_state

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto coro = [](est::loop&, est::future<int>& fut) -> est::future<int> {
      const int value = co_await fut;
      co_return value;
    };
    auto result = coro(loop, chained);
    loop.run_until_idle(); // coro suspends awaiting `chained` - still not ready

    inner_pair.reset();    // drops the inner promise and future - the inner
                           // future_state is destroyed while flatten_forwarder
                           // is still registered on it, never having run
    loop.run_until_idle(); // drains the now-completed `chained`, resuming (and
                           // finishing) the suspended coroutine

    REQUIRE(result.ready_with_failure());
    REQUIRE_THROWS_AS(result.get(), std::runtime_error);
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

// est::future<T> itself is a coroutine's return type - no separate
// task<T> wrapper - via future<T>::promise_type. Every coroutine below is
// a plain lambda still taking est::loop& as its first parameter, exactly
// as it did back when promise_type pattern-matched that first parameter
// to build its future_state<T> against. It no longer does: promise_type
// always resolves est::current_loop() now (see its own doc comment), so
// this parameter is purely vestigial - promise_type's templated
// constructor/operator new accept and silently ignore whatever arguments
// the coroutine call passes, `est::loop&` included. Left in deliberately
// (rather than mechanically stripped from every call site) as a live
// example of the ergonomic hazard this design change introduces: a
// caller can pass a loop& here that is not the current one, and nothing
// - not a compile error, not a runtime check - says so; the coroutine
// silently runs against current_loop() instead. None of these lambdas
// capture anything - state a coroutine needs crosses in as an ordinary
// by-value/by-reference parameter instead, since a capturing lambda's
// closure lives outside the coroutine frame it starts and isn't
// guaranteed to outlive it (clang-tidy's
// cppcoreguidelines-avoid-capturing-lambda-coroutines flags exactly
// this - real advice, followed here rather than suppressed, even though
// every capture below happens to be provably safe within its own test's
// scope).

TEST_CASE("a coroutine returning est::future<int> can co_return a value", "[future][coroutine]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
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
  const auto loop_guard = est::make_current_loop(loop);
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
  const auto loop_guard = est::make_current_loop(loop);
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
  const auto loop_guard = est::make_current_loop(loop);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&) -> est::future<int> {
    throw std::runtime_error("boom");
    co_return 0; // unreachable - co_return is only here to make this body a coroutine
  };

  // No co_await inside - runs synchronously, so unhandled_exception()
  // already ran by the time coro() returns.
  auto fut = coro(loop);
  REQUIRE(fut.ready());
  REQUIRE(fut.ready_with_failure());
  REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("a coroutine can co_await another coroutine's future, chaining values",
          "[future][coroutine]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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

// --- Move-only T (issue #117: "verify that a future<T> of a move only
// type works, or require some special care") ---
//
// It works, end to end - set_value()/get(), then()/then_fast(), co_await,
// and the flatten path (already covered above, "flattening a then() that
// returns a future<unique_ptr<T>> moves, not copies, the value") - with
// one real constraint a caller has to respect: a then()/then_fast()
// callback registered on a future<T> for a move-only T must take the
// value by reference (const T&, or a generic auto&/auto&& lambda), never
// by plain value. concrete_continuation<Fn, U>::run() (future.cppm)
// compiles both of its runtime branches unconditionally whenever Fn is
// also invocable with T&& - the move-out path
// (invoke_and_fulfill(std::move(state).get())), taken when this node is
// the future_state's sole owner, and the reference-read path
// (invoke_and_fulfill(state.get())), taken when it isn't - since which
// one actually runs is a runtime decision (owner_.count() == 1), not a
// compile-time one. A by-value Fn would need the reference-read path to
// copy-construct its parameter from the T& state.get() returns, which a
// move-only T can't do - so a by-value callback fails to compile, with a
// diagnostic naming then_callback_for<Fn, T> (this file's own concept
// gating then()/then_fast()) as the reason. Genuinely tried to pin this
// down with a static_assert(!requires(...)) test alongside the others
// below, the same idiom spsc_ring_tests.cpp already uses for its own
// move-only-vs-copyable checks - it doesn't work here: future<T>::then()/
// then_fast() (the outer, future<T>-level wrappers, not future_state<T>'s
// own already-concept-constrained ones they forward to) declare a
// deduced `auto` return type, so determining that return type means
// fully instantiating their body - and a body-instantiation failure is a
// hard compiler error, not a substitution failure, even from inside an
// otherwise-unevaluated requires-expression. The rejection is real and
// happens right at the then()/then_fast() call site (not buried inside
// concrete_continuation<Fn, U>), just not something this test file can
// assert on without triggering the exact hard error it would be trying
// to confirm.
//
// std::unique_ptr<int> throughout, the same move-only stand-in the
// flatten test above already uses - simple, and it would fail to compile
// the moment any of these paths silently regressed into copying.

TEST_CASE("set_value(std::move(...)) then std::move(future).get() round-trips a move-only value",
          "[future][move-only]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<std::unique_ptr<int>>();
  promise.set_value(std::make_unique<int>(42));

  REQUIRE(future.ready());
  // NOLINTNEXTLINE(bugprone-use-after-move) - false positive, see the
  // unique_ptr flatten test's own NOLINTNEXTLINE comment above.
  REQUIRE(*std::move(future).get() == 42);
}

TEST_CASE("an unwrapped then() taking const T& observes a move-only value without copying it, "
          "sole owner",
          "[future][move-only]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<std::unique_ptr<int>>();
  promise.set_value(std::make_unique<int>(1));

  int observed = 0;
  // const T&, not by value - see this section's own top comment for why a
  // move-only T requires this shape.
  (void)std::move(future).then([&](const std::unique_ptr<int>& value) { observed = *value; });
  loop.run_until_idle();

  REQUIRE(observed == 1);
}

TEST_CASE("an unwrapped then() taking const T& observes a move-only value without copying it, "
          "even when another handle keeps the future_state's ref count above 1",
          "[future][move-only]") {
  // The move-only analogue of "then() copies the stored value when
  // another handle still shares the future_state" above: here the
  // callback takes const T&, so concrete_continuation<Fn, U>::run()'s
  // count()-isn't-1 branch (a plain reference read, never a copy) is what
  // actually runs - the only shape a move-only T can take this path
  // through at all.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<std::unique_ptr<int>>();
  promise.set_value(std::make_unique<int>(2));

  int observed = 0;
  (void)future.then([&](const std::unique_ptr<int>& value) { observed = *value; });
  loop.run_until_idle();

  REQUIRE(observed == 2);
  REQUIRE(future.ready_with_value()); // `future` itself still owns its own reference, untouched
}

TEST_CASE("then() can produce a move-only value from a const-ref callback", "[future][move-only]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  auto chained = future.then([](const int& value) { return std::make_unique<int>(value + 1); });
  promise.set_value(9);
  loop.run_until_idle();

  REQUIRE(chained.ready());
  // NOLINTNEXTLINE(bugprone-use-after-move) - false positive, see the
  // unique_ptr flatten test's own NOLINTNEXTLINE comment above.
  REQUIRE(*std::move(chained).get() == 10);
}

TEST_CASE("then_fast() taking const T& observes a move-only value without copying it",
          "[future][move-only]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<std::unique_ptr<int>>();
  promise.set_value(std::make_unique<int>(7));

  int observed = 0;
  (void)std::move(future).then_fast([&](const std::unique_ptr<int>& value) { observed = *value; });

  REQUIRE(observed == 7); // then_fast() on an already-ready future runs inline, no loop drain
}

TEST_CASE("make_ready_future<T>() builds an already-ready future for a move-only T",
          "[future][move-only]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto fut = est::make_ready_future<std::unique_ptr<int>>(std::make_unique<int>(5).release());

  REQUIRE(fut.ready());
  // NOLINTNEXTLINE(bugprone-use-after-move) - false positive, see the
  // unique_ptr flatten test's own NOLINTNEXTLINE comment above.
  REQUIRE(*std::move(fut).get() == 5);
}

TEST_CASE("a coroutine can co_return and co_await a move-only value",
          "[future][move-only][coroutine]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto inner = [](est::loop&, int x) -> est::future<std::unique_ptr<int>> {
    co_return std::make_unique<int>(x * 2);
  };
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto outer = [](est::loop& loop_ref, decltype(inner)& inner_coro) -> est::future<int> {
    auto value = co_await inner_coro(loop_ref, 21);
    co_return *value + 1;
  };

  // Both coroutines run synchronously to completion here, same reason as
  // "a coroutine can co_await another coroutine's future, chaining
  // values" above - neither one ever actually suspends.
  auto fut = outer(loop, inner);
  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 43);
}

TEST_CASE("an exception in the awaited future propagates across co_await", "[future][coroutine]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, awaited] = est::make_promise_future<int>();
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
  REQUIRE(fut.ready_with_failure());
  REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("a coroutine's frame and resume nodes are all freed through the loop's allocator, "
          "no leak",
          "[future][coroutine]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    auto [promise, awaited] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
    const auto loop_guard = est::make_current_loop(loop);
    auto pair = std::optional(est::make_promise_future<int>());

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

// A coroutine's own promise_type never takes an est::loop& parameter at
// all - it always builds its future_state<T> (and allocates its own
// frame) against est::current_loop() (est:util.current_loop), whatever
// parameters the coroutine function itself declares, via the standard's
// "promise constructor arguments" rule (see promise_type's own doc
// comment). Both a coroutine that takes other parameters and one that
// takes none get their own test below, purely for coverage of that rule
// matching either shape - there is no other overload for it to be
// confused with.

TEST_CASE("a coroutine with parameters uses est::current_loop()", "[future][coroutine]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto coro = [](int value) -> est::future<int> { co_return value + 1; };

  auto fut = coro(41);
  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 42);
}

TEST_CASE("a coroutine with no parameters at all uses est::current_loop()", "[future][coroutine]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto coro = []() -> est::future<int> { co_return 42; };

  auto fut = coro();
  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 42);
}

TEST_CASE("a coroutine genuinely suspends and resumes via est::current_loop()",
          "[future][coroutine]") {
  // Not just "runs synchronously to completion" (both tests above never
  // hit a real suspension point) - this one actually suspends on a
  // not-yet-ready future and needs loop.run_until_idle() to resume it,
  // proving the frame was allocated against the *same* loop
  // current_loop() names, not some other one.
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
  auto other = future.clone();
  promise.set_value(7);
  REQUIRE(other.get() == 7);
}

TEST_CASE("both a future and its clone can register independent then() callbacks",
          "[future][clone]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<void>();
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
  const auto loop_guard = est::make_current_loop(loop);
  auto [promise, future] = est::make_promise_future<int>();
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

TEST_CASE("a completed future with no continuation ever registered can be dropped after its "
          "loop stops being current",
          "[future]") {
  // Guards future_state<T>::~future_state() checking waiters_.empty()
  // before resolving current_loop(): a future_state with nothing queued
  // needs no loop at all to be destroyed, and must not fail
  // current_loop()'s own precondition just because none happens to be
  // registered any more by the time it goes out of scope.
  std::optional<est::future<int>> outlives_the_loop;
  {
    est::loop loop;
    const auto loop_guard = est::make_current_loop(loop);
    auto [promise, future] = est::make_promise_future<int>();
    promise.set_value(42);
    outlives_the_loop.emplace(std::move(future));
  } // loop_guard exits - no loop is current from here on

  REQUIRE(outlives_the_loop->get() == 42);
  // `outlives_the_loop` (and the promise it came from) is destroyed at
  // the end of this scope, with no loop current - must not abort.
}
