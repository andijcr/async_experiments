import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Wraps the default resource, counting allocate()/deallocate() calls -
// the same small helper future_tests.cpp's own copy is, kept local to
// this file rather than shared, matching the existing convention of
// each test file being self-contained.
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

// est::mutex::lock() returns a future<lock_guard> - the lock is released
// automatically once the guard is dropped, there is no separate manual
// unlock() left to call (issue #66: unlock() is private, reachable only
// through lock_guard itself). Most tests below still drive their scenario
// through a small coroutine anyway (an ordinary lambda returning
// est::future<void> - est::future<T>'s own doc comment on operator
// co_await()/promise_type explains why a plain function works as a
// coroutine here), since that's the shape a real caller managing a
// critical section across a suspension point actually has - the guard is
// captured into a local variable and released by simply letting it go out
// of scope (the coroutine finishing, or an inner block ending), the same
// way any other RAII handle in this codebase is. None of these lambdas
// capture anything - see future_tests.cpp's own coroutine section for why
// (a capturing lambda's closure isn't guaranteed to outlive the coroutine
// frame it starts). est::loop& is the one unavoidable reference parameter
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
  const auto loop_guard = est::make_current_loop(loop);
  est::mutex m;

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&, est::mutex& mutex_ref) -> est::future<bool> {
    auto guard = co_await mutex_ref.lock();
    const bool locked_while_held = mutex_ref.locked();
    co_return locked_while_held;
  };

  // No suspension anywhere in this coroutine: initial_suspend() never
  // suspends, and lock()'s uncontended fast path returns an already-ready
  // future, which co_await also resumes inline for (future_awaiter<T>::
  // await_ready()). The whole thing runs synchronously - no
  // run_until_idle() needed to observe any of it.
  auto fut = coro(loop, m);
  REQUIRE(fut.ready());
  REQUIRE(fut.get());
  REQUIRE_FALSE(m.locked()); // the coroutine's guard was dropped on return
}

TEST_CASE("dropping the lock_guard unlocks the mutex", "[mutex]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::mutex m;

  {
    auto guard = m.lock().get();
    REQUIRE(m.locked());
  }
  REQUIRE_FALSE(m.locked());
}

TEST_CASE("moving a lock_guard transfers ownership of the unlock", "[mutex]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::mutex m;

  auto first = m.lock().get();
  {
    auto second = std::move(first);
    REQUIRE(m.locked());
    // `second` goes out of scope here and unlocks - `first`, moved-from,
    // must not also try to (it would be a double-unlock, handing the lock
    // to a waiter twice or clearing an already-clear count()).
  }
  REQUIRE_FALSE(m.locked());
}

// A long, deliberately linear sequence of independent steps (create two
// coroutines, drive each to its own suspension point, then release and
// check final state), not deep branching/nesting - splitting it into
// several TEST_CASEs would only scatter one coherent scenario.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("a second co_await lock() suspends until the first coroutine unlocks", "[mutex]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::mutex m;
  auto [release_promise, release_future] = est::make_promise_future<void>();
  std::vector<int> order;

  // Acquires, records itself, then stays suspended (holding the guard)
  // until the test explicitly completes `release` - the controlled
  // stand-in for "some other work happens while the lock is held" that
  // lets this test observe contention deterministically, without a real
  // or fake clock. The guard drops (releasing the lock) once the
  // coroutine finishes, right after `release` resolves.
  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto first = [](est::loop&,
                  est::mutex& mutex_ref,
                  std::vector<int>& order_ref,
                  est::future<void> release) -> est::future<void> {
    auto guard = co_await mutex_ref.lock();
    order_ref.push_back(1);
    co_await std::move(release);
    co_return; // guard drops here, releasing the lock
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto second =
      [](est::loop&, est::mutex& mutex_ref, std::vector<int>& order_ref) -> est::future<void> {
    auto guard = co_await mutex_ref.lock();
    order_ref.push_back(2);
    co_return; // guard drops here
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
  loop.run_until_idle(); // first resumes, drops its guard (handing off to second), second finishes

  REQUIRE(order == std::vector{1, 2});
  REQUIRE_FALSE(m.locked());
  REQUIRE_FALSE(m.has_waiters());
  REQUIRE(fut1.ready());
  REQUIRE(fut2.ready());
}

// Same justification as the TEST_CASE above.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("unlock() resumes queued waiters in FIFO order", "[mutex]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::mutex m;
  auto [release_promise, release_future] = est::make_promise_future<void>();
  std::vector<int> order;

  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto holder =
      [](est::loop&, est::mutex& mutex_ref, est::future<void> release) -> est::future<void> {
    auto guard = co_await mutex_ref.lock();
    co_await std::move(release);
    co_return; // guard drops here, releasing the lock
  };
  auto waiter = [](est::loop&,
                   est::mutex& mutex_ref,
                   std::vector<int>& order_ref,
                   int id) -> est::future<void> {
    auto guard = co_await mutex_ref.lock();
    order_ref.push_back(id);
    co_return; // guard drops here
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

  holder(loop, m, std::move(release_future));
  loop.run_until_idle(); // holder acquires, then suspends on `release`

  // Each waiter is created and run to its own suspension point one at a
  // time (rather than all three back to back before a single
  // run_until_idle()) so this test's own call order maps directly onto
  // enqueue order into the mutex's waiter list, without also having to
  // reason about est::loop's ready-queue's own drain order among several
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

  // FIFO: the first-queued waiter (1) is resumed first, all the way up
  // to the last queued (3) - est::intrusive_list's documented order,
  // unchanged by lock handoff.
  REQUIRE(order == std::vector{1, 2, 3});
  REQUIRE_FALSE(m.locked());
  REQUIRE_FALSE(m.has_waiters());
  REQUIRE(fut1.ready());
  REQUIRE(fut2.ready());
  REQUIRE(fut3.ready());
}

TEST_CASE("lock() on an unlocked mutex returns an already-ready future", "[mutex]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::mutex m;

  auto fut = m.lock();
  REQUIRE(fut.ready()); // fast path - completes the promise immediately
  REQUIRE(m.locked());

  auto guard = std::move(fut).get();
  REQUIRE(m.locked()); // still held - guard is alive
  (void)guard;
}

TEST_CASE("lock() on a locked mutex defers until the holder's guard is dropped", "[mutex]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::mutex m;

  auto holder = m.lock().get();
  auto fut = m.lock();
  REQUIRE_FALSE(fut.ready()); // queued - unlike the fast path above
  REQUIRE(m.has_waiters());

  loop.run_until_idle(); // nothing to drain yet - unlock() hasn't run
  REQUIRE_FALSE(fut.ready());

  // Moving `holder` into a block-scoped variable and letting it go out of
  // scope is the point: its destructor is what releases the lock.
  {
    auto dropped = std::move(holder);
  }
  loop.run_until_idle(); // the waiter's continuation runs, completing `fut`
  REQUIRE(fut.ready());
  auto second = std::move(fut).get();
  REQUIRE(m.locked());
  (void)second;
}

TEST_CASE("lock() can be co_await'ed from inside a coroutine", "[mutex]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::mutex m;

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto coro = [](est::loop&, est::mutex& mutex_ref) -> est::future<bool> {
    auto guard = co_await mutex_ref.lock();
    const bool locked_while_held = mutex_ref.locked();
    co_return locked_while_held;
  };

  auto fut = coro(loop, m);
  loop.run_until_idle();
  REQUIRE(fut.ready());
  REQUIRE(fut.get());
  REQUIRE_FALSE(m.locked()); // the coroutine's guard was dropped on return
}

TEST_CASE("destroying a mutex with a future<lock_guard> still queued on lock() leaks nothing",
          "[mutex]") {
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    est::mutex m;

    auto holder = m.lock().get(); // never dropped - holds the lock forever
    auto waiter_fut = m.lock();
    REQUIRE_FALSE(waiter_fut.ready());
    REQUIRE(m.has_waiters());
    // `m` (and `loop`) are destroyed at the end of this scope with
    // `waiter_fut`'s continuation still queued, never resumed.
    (void)holder;
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("destroying a mutex with a coroutine co_await-ing lock() still pending leaks nothing",
          "[mutex]") {
  // A sharper version of the leak test above: there, the pending waiter
  // was a plain future<lock_guard> checked directly, never co_await'ed -
  // its own continuation node's promise_ was the *only* owner of its
  // future_state<lock_guard>, so dropping it (via ~mutex()'s drain) was
  // always enough to free everything. A coroutine suspended via
  // co_await, by contrast, holds its own reference to that same
  // future_state (the future<lock_guard> temporary co_await awaits is
  // spilled into the coroutine's own frame across the suspension) - so
  // dropping *just* that continuation's own reference leaves the
  // future_state (and the coroutine frame keeping it alive) with nowhere
  // left to go, unless abandon() actually completes the promise (with an
  // exception, here) instead of silently dropping it - see
  // detail::promise_resume_node's own doc comment (est/src/promise.cppm)
  // for the full reasoning est::mutex now inherits by building on
  // est::binary_event. Without that fix, this test leaks the waiting
  // coroutine's entire frame.
  counting_resource resource;
  {
    est::loop loop{&resource};
    const auto loop_guard = est::make_current_loop(loop);
    est::mutex m;

    auto holder = m.lock().get(); // never dropped - holds the lock forever

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto coro = [](est::loop&, est::mutex& mutex_ref) -> est::future<void> {
      auto guard = co_await mutex_ref.lock();
      co_return; // never reached - holder above never unlocks
    };
    auto waiter_fut = coro(loop, m);
    loop.run_until_idle(); // waiter finds it locked, suspends, queued in the mutex's waiters

    REQUIRE_FALSE(waiter_fut.ready());
    REQUIRE(m.has_waiters());
    (void)holder;
    // `m` (and `loop`) are destroyed at the end of this scope with the
    // coroutine still suspended in co_await mutex_ref.lock(), never resumed.
  }
  REQUIRE(resource.allocations > 0);
  REQUIRE(resource.allocations == resource.deallocations);
}

TEST_CASE("mutex() default-constructs and uses est::current_loop()", "[mutex]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::mutex m;
  REQUIRE_FALSE(m.locked());

  auto fut = m.lock();
  REQUIRE(fut.ready());
  auto held = std::move(fut).get();
  REQUIRE(m.locked());
  (void)held;
}

TEST_CASE("an unlocked mutex with no waiters can be dropped after its loop stops being current",
          "[mutex]") {
  // Guards mutex::~mutex() (really counting_event<Mode>::~counting_event(),
  // which mutex's own event_ member inherits) checking has_waiters()
  // before resolving current_loop(): a mutex with nothing queued needs no
  // loop at all to be destroyed, and must not fail current_loop()'s own
  // precondition just because none happens to be registered any more by
  // the time it goes out of scope.
  est::mutex m;
  {
    est::loop loop;
    const auto loop_guard = est::make_current_loop(loop);
    (void)m.lock().get(); // uncontended fast path; the returned lock_guard is
                          // a temporary - dropped immediately, releasing the
                          // lock right here (same effect an explicit
                          // unlock() call would have had, back when one was
                          // public) - (void) discards the [[nodiscard]]
                          // lock_guard explicitly, same as every other
                          // deliberately-unused lock_guard in this file
  } // loop_guard exits - no loop is current from here on

  REQUIRE_FALSE(m.locked());
  // `m` is destroyed at the end of this scope with no loop current - must
  // not abort.
}
