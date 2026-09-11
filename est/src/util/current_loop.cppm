export module est:util.current_loop;

import :check;
import :loop;
import :platform;
import :util.scope_exit;

// make_current_loop()'s own nesting guard - deliberately *not* the same
// signal as platform::interface::get_current_loop_context()/
// set_current_loop_context() (the pair current_loop() itself reads from):
// hosted_stdcpp's implementation of that pair (module estext) falls back
// to a loop of its own whenever nothing has been explicitly registered,
// so "is get_current_loop_context() non-null" can't answer "has some
// loop already called make_current_loop()?" - it's always non-null once
// that fallback exists. Only this flag, tracked independently of
// :platform's own storage, can still answer that question. A plain
// (deliberately not thread_local) bool - est::loop is driven from one
// call stack, same as everything else in this codebase.
namespace est::detail {
inline bool loop_is_current = false;
}

// The opt-in "current loop" mechanism: what make_promise_future(),
// sleep_for()/sleep_until()/yield_execution(), and a loop-less
// coroutine's own promise_type (est:future) all fall back to when called
// without an explicit loop&. Deliberately free functions, not members of
// est::loop itself - creating a loop and deciding whether it should
// become "the" current one are two separate concerns, and most loops in
// this codebase's own tests are never meant to be "the" current loop at
// all. Keeping this mechanism out of loop.cppm means est::loop's own
// constructor/destructor stay the plain, platform-agnostic primitive
// they've always been.
export namespace est {

// Marks `loop_ref` as the loop make_promise_future()/sleep_for()/
// sleep_until()/yield_execution()/a loop-less coroutine's promise_type
// (all consuming current_loop(), below) fall back to, until the returned
// guard is destroyed - modeled on est::platform::override_instance()'s
// own RAII shape (built the same way, on est::scope_exit).
//
// A single slot with a checked precondition against nesting, not a
// push/pop RAII stack like override_instance()'s: two loops both current
// at once is treated as a programming error rather than "the inner one
// temporarily shadows the outer" - simpler to reason about, and nothing
// in this codebase has a legitimate reason to nest this way.
//
// The returned guard's cleanup lambda captures nothing (not even a
// pointer to loop_ref): it unconditionally clears the slot back to
// nullptr rather than reading anything off of loop_ref, so it stays
// correct even if the guard somehow outlived the loop it was made from.
[[nodiscard]] auto make_current_loop(loop& loop_ref) noexcept {
  check(!detail::loop_is_current,
        "est::make_current_loop(): another loop is already current - only one "
        "loop can be current at a time");
  detail::loop_is_current = true;
  platform::instance().set_current_loop_context(&loop_ref);
  return scope_exit([]() noexcept {
    detail::loop_is_current = false;
    platform::instance().set_current_loop_context(nullptr);
  });
}

// The current loop: what make_promise_future(), sleep_for()/
// sleep_until()/yield_execution(), and a loop-less coroutine's own
// promise_type (est:future) each fall back to when called without an
// explicit loop&. Precondition (checked): a loop must actually be
// current - call make_current_loop() on one first, or pass a loop&
// explicitly. In practice, hosted_stdcpp's own
// get_current_loop_context() (module estext) never fails this check: it
// falls back to a loop of its own when nothing has been explicitly
// registered. A test fake (est/tests/) generally doesn't provide that
// fallback, so this stays a real, reachable precondition under one.
[[nodiscard]] auto current_loop() -> loop& {
  auto* const context = platform::instance().get_current_loop_context();
  check(context != nullptr,
        "est::current_loop(): no loop is current - call make_current_loop() on one "
        "first, or pass a loop& explicitly instead of relying on the implicit one");
  return *context;
}

} // namespace est
