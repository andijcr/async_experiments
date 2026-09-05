import est;
import std;

#include <catch2/catch_test_macros.hpp>

// est::mutex::lock() is awaitable-only (M4, docs/PLAN.md): it can no
// longer be called synchronously the way earlier versions of this file
// did (`m.lock();` as a plain statement) - the only way to acquire it is
// `co_await mutex.lock();` from inside a coroutine. Every test below
// therefore drives its scenario through a small coroutine (an ordinary
// lambda returning est::future<void> - est::future<T>'s own doc comment
// on operator co_await()/promise_type explains why a plain function
// works as a coroutine here) rather than calling lock()/unlock()
// directly from the TEST_CASE body itself, which - not being a coroutine
// - cannot co_await anything. None of these lambdas capture anything -
// see future_tests.cpp's own coroutine section for why (a capturing
// lambda's closure isn't guaranteed to outlive the coroutine frame it
// starts). est::loop& is the one unavoidable reference parameter
// (required by future<T>::promise_type's own calling convention) -
// NOLINT'd per declaration, matching the same already-accepted "a loop&
// is safe given its own documented lifetime precondition" stance.
//
// A coroutine returning est::future<T> never runs any of its body inline
// on the call that creates it (future<T>::promise_type::initial_suspend(),
// est:future) - calling one only enqueues its first resumption onto the
// est::loop passed as its first parameter. loop.run_until_idle() is what
// actually runs it, same as every other future_state<T> in this codebase.

TEST_CASE("co_await lock() acquires immediately when unlocked", "[mutex]") {
  est::loop loop;
  est::mutex m(loop);

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&, est::mutex& mutex_ref) -> est::future<void> {
    co_await mutex_ref.lock();
    co_return;
  };

  auto fut = coro(loop, m);
  REQUIRE_FALSE(m.locked()); // deferred - nothing has run yet
  loop.run_until_idle();
  REQUIRE(fut.ready());
  REQUIRE(m.locked());
  REQUIRE_FALSE(m.has_waiters());
}

TEST_CASE("unlock() releases the lock when nothing is waiting", "[mutex]") {
  est::loop loop;
  est::mutex m(loop);

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&, est::mutex& mutex_ref) -> est::future<void> {
    co_await mutex_ref.lock();
    mutex_ref.unlock();
    co_return;
  };

  coro(loop, m);
  loop.run_until_idle();
  REQUIRE_FALSE(m.locked());
  REQUIRE_FALSE(m.has_waiters());
}

// A long, deliberately linear sequence of independent steps (create two
// coroutines, drive each to its own suspension point, then release and
// check final state), not deep branching/nesting - splitting it into
// several TEST_CASEs would only scatter one coherent scenario.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("a second co_await lock() suspends until the first coroutine unlocks", "[mutex]") {
  est::loop loop;
  est::mutex m(loop);
  auto [release_promise, release_future] = est::make_promise_future<void>(loop);
  std::vector<int> order;

  // Acquires, records itself, then stays suspended (holding the lock)
  // until the test explicitly completes `release` - the controlled
  // stand-in for "some other work happens while the lock is held" that
  // lets this test observe contention deterministically, without a real
  // or fake clock.
  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto first = [](est::loop&,
                  est::mutex& mutex_ref,
                  std::vector<int>& order_ref,
                  est::future<void> release) -> est::future<void> {
    co_await mutex_ref.lock();
    order_ref.push_back(1);
    co_await std::move(release);
    mutex_ref.unlock();
    co_return;
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto second =
      [](est::loop&, est::mutex& mutex_ref, std::vector<int>& order_ref) -> est::future<void> {
    co_await mutex_ref.lock();
    order_ref.push_back(2);
    mutex_ref.unlock();
    co_return;
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

  auto fut1 = first(loop, m, order, std::move(release_future));
  loop.run_until_idle(); // first acquires, then suspends on `release`
  REQUIRE(m.locked());
  REQUIRE(order == std::vector{1});

  auto fut2 = second(loop, m, order);
  loop.run_until_idle(); // second finds it locked, suspends without recording itself
  REQUIRE(order == std::vector{1});
  REQUIRE(m.has_waiters());
  REQUIRE(m.locked()); // still held by `first`, unchanged by the failed fast path

  release_promise.set_value();
  loop.run_until_idle(); // first resumes, unlocks (handing off to second), second finishes

  REQUIRE(order == std::vector{1, 2});
  REQUIRE_FALSE(m.locked());
  REQUIRE_FALSE(m.has_waiters());
  REQUIRE(fut1.ready());
  REQUIRE(fut2.ready());
}

// Same justification as the TEST_CASE above.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("unlock() resumes queued waiters in LIFO order", "[mutex]") {
  est::loop loop;
  est::mutex m(loop);
  auto [release_promise, release_future] = est::make_promise_future<void>(loop);
  std::vector<int> order;

  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto holder =
      [](est::loop&, est::mutex& mutex_ref, est::future<void> release) -> est::future<void> {
    co_await mutex_ref.lock();
    co_await std::move(release);
    mutex_ref.unlock();
    co_return;
  };
  auto waiter = [](est::loop&,
                   est::mutex& mutex_ref,
                   std::vector<int>& order_ref,
                   int id) -> est::future<void> {
    co_await mutex_ref.lock();
    order_ref.push_back(id);
    mutex_ref.unlock();
    co_return;
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

  holder(loop, m, std::move(release_future));
  loop.run_until_idle(); // holder acquires, then suspends on `release`

  // Each waiter is created and run to its own suspension point one at a
  // time (rather than all three back to back before a single
  // run_until_idle()) so this test's own call order maps directly onto
  // enqueue order into mutex's waiter list, without also having to
  // reason about est::loop's ready-queue's own LIFO order among several
  // freshly-created, not-yet-started coroutines.
  auto fut1 = waiter(loop, m, order, 1);
  loop.run_until_idle();
  auto fut2 = waiter(loop, m, order, 2);
  loop.run_until_idle();
  auto fut3 = waiter(loop, m, order, 3);
  loop.run_until_idle();

  REQUIRE(order.empty());
  REQUIRE(m.has_waiters());

  release_promise.set_value();
  loop.run_until_idle();

  // LIFO: the most recently queued waiter (3) is resumed first, all the
  // way down to the first queued (1) - est::intrusive_list's documented
  // order, unchanged by lock handoff.
  REQUIRE(order == std::vector{3, 2, 1});
  REQUIRE_FALSE(m.locked());
  REQUIRE_FALSE(m.has_waiters());
  REQUIRE(fut1.ready());
  REQUIRE(fut2.ready());
  REQUIRE(fut3.ready());
}
