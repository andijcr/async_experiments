import est;
import std;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("stop_source: stop_requested() is false before request_stop()", "[stop_token]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  REQUIRE_FALSE(source.stop_requested());
  REQUIRE_FALSE(source.get_token().stop_requested());
}

TEST_CASE("stop_source: request_stop() is observed by stop_requested() on both the source and "
          "its token",
          "[stop_token]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  auto token = source.get_token();

  source.request_stop();

  REQUIRE(source.stop_requested());
  REQUIRE(token.stop_requested());
}

TEST_CASE("stop_source: request_stop() is idempotent - a second call is a no-op, not a checked "
          "failure",
          "[stop_token]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;

  source.request_stop();
  source.request_stop(); // must not abort - matches one_shot_event::set()'s own contract
  source.request_stop();

  REQUIRE(source.stop_requested());
}

TEST_CASE("stop_source: get_token() called more than once returns independent handles that all "
          "observe one request_stop()",
          "[stop_token]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  auto first = source.get_token();
  auto second = source.get_token();

  REQUIRE_FALSE(first.stop_requested());
  REQUIRE_FALSE(second.stop_requested());

  source.request_stop();

  REQUIRE(first.stop_requested());
  REQUIRE(second.stop_requested());
}

TEST_CASE("stop_token::stopped() resolves immediately when already stop_requested()",
          "[stop_token]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  source.request_stop();

  auto fut = source.get_token().stopped();
  REQUIRE(fut.ready());
  REQUIRE_FALSE(fut.failed());
}

TEST_CASE("stop_token::stopped() is unready until request_stop() is called, then becomes ready "
          "synchronously - matching promise<void>::set_value()'s own synchronous ready() "
          "transition",
          "[stop_token]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;

  auto fut = source.get_token().stopped();
  REQUIRE_FALSE(fut.ready());

  source.request_stop(); // request_stop() completes the promise directly, not through
  REQUIRE(fut.ready());  // a loop-scheduled node - no run_until_idle() needed here
}

TEST_CASE("stop_token::stopped(): two independent tokens each get their own future, both "
          "resolved by one request_stop()",
          "[stop_token]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;

  auto first = source.get_token().stopped();
  auto second = source.get_token().stopped();
  REQUIRE_FALSE(first.ready());
  REQUIRE_FALSE(second.ready());

  source.request_stop();

  REQUIRE(first.ready());
  REQUIRE(second.ready());
}

TEST_CASE("a coroutine can co_await stop_token::stopped(), genuinely suspending until "
          "request_stop()",
          "[stop_token][coroutine]") {
  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  est::stop_source source;
  bool resumed = false;

  // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
  auto waiter = [](est::stop_token token, bool& resumed_ref) -> est::future<void> {
    co_await token.stopped();
    resumed_ref = true;
    co_return;
  };
  // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

  auto fut = waiter(source.get_token(), resumed);
  loop.run_until_idle();
  REQUIRE_FALSE(resumed);

  source.request_stop();
  loop.run_until_idle();

  REQUIRE(resumed);
  REQUIRE(fut.ready());
}
