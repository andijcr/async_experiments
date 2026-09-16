import est;
import std;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("with_stop(): normal completion (token never fires) forwards the operation's own value",
          "[with_stop]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  auto [prom, operation] = est::make_promise_future<int>();

  auto fut = est::with_stop(std::move(operation), source.get_token());
  REQUIRE_FALSE(fut.ready());

  prom.set_value(42);
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE_FALSE(fut.ready_with_failure());
  REQUIRE(fut.get() == 42);
}

TEST_CASE("with_stop(): normal completion forwards the operation's own failure unchanged",
          "[with_stop]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  auto [prom, operation] = est::make_promise_future<int>();

  auto fut = est::with_stop(std::move(operation), source.get_token());
  prom.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE(fut.ready_with_failure());
  REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("with_stop(): request_stop() before the operation completes resolves with "
          "operation_cancelled, and the operation's own later completion is a no-op",
          "[with_stop]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  auto [prom, operation] = est::make_promise_future<int>();

  auto fut = est::with_stop(std::move(operation), source.get_token());
  REQUIRE_FALSE(fut.ready());

  source.request_stop();
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE(fut.ready_with_failure());
  REQUIRE_THROWS_AS(fut.get(), est::operation_cancelled);

  // The operation was cancelled from the caller's point of view, but
  // nothing eagerly freed it (with_stop()'s own documented limitation) -
  // it's still perfectly valid to complete the promise driving it. That
  // completion must reach `fut`'s already-`done`-guarded first racer and
  // be silently dropped, not double-complete `fut` (which would trip
  // future_state<T>::check_not_completed()'s assertion).
  prom.set_value(7);
  loop.run_until_idle();

  REQUIRE_THROWS_AS(fut.get(), est::operation_cancelled); // unchanged
}

TEST_CASE("with_stop(): an already-stop_requested() token short-circuits to operation_cancelled "
          "without ever completing the operation",
          "[with_stop]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  source.request_stop();
  auto [prom, operation] = est::make_promise_future<int>();

  auto fut = est::with_stop(std::move(operation), source.get_token());

  REQUIRE(fut.ready());
  REQUIRE(fut.ready_with_failure());
  REQUIRE_THROWS_AS(fut.get(), est::operation_cancelled);

  // `operation` was never registered against - dropping `prom` unfulfilled
  // here (end of scope) must not crash.
}

TEST_CASE("with_stop(): request_stop() after the operation has already completed is a no-op - "
          "the result stays the operation's own value",
          "[with_stop]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  auto [prom, operation] = est::make_promise_future<int>();

  auto fut = est::with_stop(std::move(operation), source.get_token());
  prom.set_value(42);
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 42);

  // The operation already won the race - a later request_stop() must reach
  // the second racer's own `done`-guard and be silently dropped, not
  // overwrite an already-completed result.
  source.request_stop();
  loop.run_until_idle();

  REQUIRE(fut.get() == 42); // unchanged
}

TEST_CASE("with_stop<void>(): normal completion forwards success", "[with_stop][void]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  auto [prom, operation] = est::make_promise_future<void>();

  auto fut = est::with_stop(std::move(operation), source.get_token());
  prom.set_value();
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE_FALSE(fut.ready_with_failure());
}
