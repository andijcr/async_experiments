export module est:util.current_loop;

import std;
import :check;
import :loop;
import :util.scope_exit;

// make_current_loop()'s own thread_local storage: the registered loop,
// and its allocator's memory_resource cached directly alongside it so
// current_allocator() below doesn't need to dereference through the loop
// pointer just to reach it - one thread_local load instead of one
// thread_local load plus a further memory read through it. Both null
// when nothing is registered on this thread/core; current_loop()/
// current_allocator() check that explicitly rather than relying on a
// caller never touching either too early.
//
// thread_local, not a single process-global: this codebase's ultimate
// target is a shared-memory, no-MMU multicore machine with one loop per
// core - a plain global would need every core to agree on (and
// synchronize writes to) one slot, even though each core only ever
// registers and reads its own loop. thread_local gives each core (or,
// hosted, each thread) an independent slot for the price of one
// relative-addressed load, no synchronization needed - the same reasoning
// est::platform::detail::current_instance (:platform) now applies to its
// own storage.
namespace est::detail {

struct execution_context {
  loop* loop_ptr = nullptr;
  std::pmr::memory_resource* resource_ptr = nullptr;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline thread_local execution_context tls_context;

} // namespace est::detail

// The opt-in "current loop" mechanism: what make_promise_future(),
// sleep_for()/sleep_until()/yield_execution(), est::mutex,
// est::counting_event<Mode>, and a loop-less coroutine's own promise_type
// all resolve when called without an explicit loop&. Deliberately free
// functions, not members of est::loop itself - creating a loop and
// deciding whether it should become "the" current one are two separate
// concerns, and most loops in this codebase's own tests are never meant
// to be "the" current loop at all. Keeping this mechanism out of
// loop.cppm means est::loop's own constructor/destructor stay the plain,
// platform-agnostic primitive they've always been.
//
// Deliberately kept separate from est::platform::override_instance()
// (:platform) rather than merged into one combined "install everything"
// call: a caller that only wants to override the platform backend (most
// of est/tests/platform_tests.cpp and timer_tests.cpp, testing
// est::check()/est::timer_queue in isolation) has no loop to register at
// all, and a caller that overrides the platform once for an entire
// program/test binary but registers a different loop per operation
// (est/tests/loop_tests.cpp) would otherwise have to re-supply the same
// platform reference every time. Two independent, composable scoped
// registrations fit both shapes; one combined one fits neither as well.
export namespace est {

// Marks `loop_ref` as the loop make_promise_future()/sleep_for()/
// sleep_until()/yield_execution()/est::mutex/est::counting_event<Mode>/a
// loop-less coroutine's promise_type (all consuming current_loop()/
// current_allocator(), below) resolve, until the returned guard is
// destroyed - modeled on est::platform::override_instance()'s own RAII
// shape (built the same way, on est::scope_exit).
//
// A single slot with a checked precondition against nesting, not a
// push/pop RAII stack like override_instance()'s: two loops both current
// at once (on the same thread/core) is treated as a programming error
// rather than "the inner one temporarily shadows the outer" - simpler to
// reason about, and nothing in this codebase has a legitimate reason to
// nest this way.
//
// The returned guard's cleanup lambda calls loop_ref.drain_pending()
// (est:loop) before clearing the slot, not just after - draining anything
// loop_ref still has queued while it's still the registered current loop,
// rather than leaving that for loop_ref's own destructor to find later,
// possibly under a different (or no) current loop by then. Destroying a
// still-suspended coroutine's resume node can itself need to destroy that
// coroutine's frame, and a coroutine frame's operator delete always
// resolves est::current_allocator() fresh (detail::coroutine_frame_dealloc(),
// est:future) rather than caching an allocator of its own - draining only
// after this slot is cleared would resolve that lookup against whatever
// loop is current *next* instead of loop_ref, silently deallocating
// through the wrong loop's allocator. This does mean the lambda captures
// loop_ref now (by reference, not by value) rather than nothing at all:
// the guard must not outlive the loop it was made from, the same
// precondition every other loop& in this codebase already carries.
[[nodiscard]] auto make_current_loop(loop& loop_ref) noexcept {
  check(detail::tls_context.loop_ptr == nullptr,
        "est::make_current_loop(): another loop is already current - only one "
        "loop can be current at a time");
  detail::tls_context = {.loop_ptr = &loop_ref, .resource_ptr = loop_ref.allocator().resource()};
  return scope_exit([&loop_ref]() noexcept {
    loop_ref.drain_pending();
    detail::tls_context = {};
  });
}

// The current loop: what make_promise_future(), sleep_for()/
// sleep_until()/yield_execution(), est::mutex, est::counting_event<Mode>,
// and a loop-less coroutine's own promise_type each resolve when called
// without an explicit loop&. Precondition (checked): a loop must actually
// be current on this thread/core - call make_current_loop() on one first,
// or pass a loop& explicitly instead. Unlike an earlier version of this
// mechanism, there is no implicit fallback to some default loop when
// nothing is registered - every caller (hosted or bare-metal) must
// register one explicitly.
[[nodiscard]] auto current_loop() -> loop& {
  check(detail::tls_context.loop_ptr != nullptr,
        "est::current_loop(): no loop is current - call make_current_loop() on one "
        "first, or pass a loop& explicitly instead of relying on the implicit one");
  return *detail::tls_context.loop_ptr;
}

// current_loop().allocator(), without the loop-pointer dereference in
// between: the memory_resource* make_current_loop() cached is read
// directly out of thread-local storage instead. Every internal caller
// that only needs an allocator, not the loop itself (future_state<T>'s
// own allocator(), a coroutine frame's operator new/delete, ...), uses
// this instead of current_loop().allocator() for exactly that reason -
// see est:future's own doc comments on coroutine_frame_alloc()/
// coroutine_frame_dealloc() for why the two can resolve to different
// loops if the current-loop registration changes between related calls,
// the same hazard current_loop() itself carries.
[[nodiscard]] auto current_allocator() -> std::pmr::polymorphic_allocator<std::byte> {
  check(detail::tls_context.resource_ptr != nullptr,
        "est::current_allocator(): no loop is current - call make_current_loop() on one "
        "first, or pass an allocator explicitly instead of relying on the implicit one");
  return {detail::tls_context.resource_ptr};
}

} // namespace est
