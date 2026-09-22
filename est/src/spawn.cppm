export module est:spawn;

import std;
import :future;
import :loop;
import :platform;
import :sync.stop_token;

// Issue #58: dispatching a coroutine without awaiting it (fire-and-forget,
// letting a later .then() continuation observe the result) used to mean
// discarding the returned future<T> and relying on the fact that a
// continuation node queued on it keeps the underlying future_state<T>
// alive on its own - a real but non-obvious invariant that had to be
// explained in a comment at every call site (examples/spreadsheet/main.cpp's
// own run_server() was the motivating case). That invariant turned out to
// already be sufficient on its own for the coroutine to actually run:
// future<T>::promise_type's initial_suspend()/final_suspend() are both
// std::suspend_never (est:future), so a coroutine starts immediately on
// call and frees its own frame the instant it finishes, independent of
// whether the caller kept the returned future<T> at all. The real,
// narrower gap spawn() closes is different: an unobserved *failure* was
// silently dropped - future_state<T>::then()'s unwrapped-mode
// auto-propagate path just routes it into a downstream future_state that's
// also discarded, so a genuine bug (a bad_alloc, a logic error) produced
// total silence instead of a diagnostic.
//
// spawn() is deliberately just sugar over then_fast() - no separate
// loop-owned tracking collection. An earlier version of this file did add
// one (detail::spawned_entry/loop::track_spawned()/untrack_spawned(),
// mirroring loop::pending_timers_'s own shape), on the theory that
// fire-and-forget dispatch deserved an "explicit, loop-owned reason to
// stay alive" distinct from the implicit one every other combinator here
// already relies on. That turned out not to be a real distinction: the
// then_fast() continuation registered below already keeps future_state<T>
// alive via its own owner_ reference, exactly like with_stop()/
// with_timeout()/when_all()/when_any()/when_any_succeeds() already do -
// none of them track anything extra either, and the coroutine's own
// suspend_never initial/final suspend (above) means correctness never
// depended on the tracking layer in the first place. Its only real
// payoff was loop::spawned_count(), a "how many spawned tasks are
// currently in flight" diagnostic nothing in this codebase reads except
// tests - not worth ~60 lines of type-erased base class, concrete
// wrapper, and find_if-based bookkeeping to keep around speculatively
// (CLAUDE.md's own "don't design for hypothetical future requirements").
// Revisit if issue #103's own central waiter registry ever lands and
// wants spawn() as a real consumer of it, rather than reintroducing this
// same tracking shape ad hoc.
//
// spawn() also no longer takes an explicit loop& (PR #122 review): a
// then_fast() continuation dispatches through whatever loop already owns
// `task`'s own future_state (established when `task` was created, not by
// spawn() itself), so spawn() never needed a loop& to do its own job in
// the first place.
//
// The exception hook (below) is a thread_local, not something stored on
// loop at all (also PR #122 review): reporting an unhandled exception
// from a spawned task is a per-thread policy - which platform::printdbg()
// to call, which exceptions are routine - not a property of any one loop
// object, and this codebase already has exactly one thread_local "current
// loop" per thread/core (:util.current_loop) to begin with, so tying the
// hook to a specific loop instance bought nothing. Storing it thread_local
// here also means it's statically initialized to
// default_spawn_exception_hook (below) once per thread, for free - no
// registration step, no "still empty" state to self-heal or guard against
// at all. An earlier version of this file instead stored the hook on
// est::loop (loop::exception_hook_type/set_spawn_exception_hook()/
// spawn_exception_hook()) and needed a whole registration wrapper
// (est::make_current_loop_with_spawn()) plus a spawn()-side est::check()
// precondition just to keep it non-empty - both gone now, along with the
// coupling to loop that made them necessary.

export namespace est {

// The type est::set_spawn_exception_hook()/est::spawn_exception_hook()
// (below) traffic in - std::function, not std::move_only_function,
// matching every other std::function-shaped customization point in this
// codebase (est::loop::scheduler_type's own doc comment has the reasoning:
// a hook a caller may want to copy into more than one place, unlike a
// single-owner completion callback).
using spawn_exception_hook_type = std::function<void(const std::exception_ptr&)>;

// The default hook spawn() installs when a caller hasn't set their own via
// est::set_spawn_exception_hook() (below): a best-effort diagnostic via
// platform::instance()'s own sink (platform::printdbg()) - loud enough
// that a genuine bug in a fire-and-forget coroutine doesn't vanish in
// total silence (issue #58), but not a hard est::check() failure, since a
// caller may legitimately want different behavior (structured logging,
// metrics, or even to treat this as fatal) - install a different hook via
// est::set_spawn_exception_hook() instead of editing this one.
//
// operation_cancelled (est:sync.stop_token) and detail::abandoned_exception
// (est:loop) are deliberately exempt: both are routine, expected outcomes
// of normal cancellation/shutdown, not bugs - warning about them by
// default would spam every ordinary teardown. This exemption applies only
// to this built-in default; a caller-installed hook sees every exception
// unfiltered and decides for itself.
inline void default_spawn_exception_hook(const std::exception_ptr& eptr) noexcept {
#ifdef __cpp_exceptions
  try {
    std::rethrow_exception(eptr);
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (const operation_cancelled&) {
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (const detail::abandoned_exception&) {
  } catch (const std::exception& e) {
    platform::printdbg("est::spawn(): unhandled exception: {}", e.what());
  } catch (...) {
    platform::printdbg("est::spawn(): unhandled exception (not a std::exception)");
  }
#else
  // -fno-exceptions: nothing here can meaningfully rethrow/catch to tell
  // operation_cancelled/abandoned_exception apart from a real bug - report
  // unconditionally rather than silently dropping every unhandled
  // exception outright.
  (void)eptr;
  platform::printdbg("est::spawn(): unhandled exception");
#endif
}

} // namespace est

namespace est::detail {

// thread_local, not a single process-global, matching :util.current_loop's
// own tls_context (same reasoning: this codebase's ultimate target is one
// loop per core on a shared-memory, no-MMU multicore machine, and each
// core only ever touches its own hook). Statically initialized to
// default_spawn_exception_hook - never empty from the moment this thread
// starts, so nothing downstream needs to guard against a "not yet
// installed" state.
//
// bugprone-throwing-static-initialization flags std::function's own
// constructor as potentially-throwing in general (an allocation for a
// target too large for its small-object buffer) - not a real risk here:
// the target is a plain, captureless function pointer, always small
// enough for std::function's guaranteed SBO, so this construction never
// allocates and can't throw in practice.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
inline thread_local spawn_exception_hook_type spawn_exception_hook_storage =
    default_spawn_exception_hook;

} // namespace est::detail

export namespace est {

// Installs (or resets) this thread's spawn() exception hook. Passing an
// empty/null `hook` resets to default_spawn_exception_hook() above rather
// than leaving the slot empty - the same "reset to default, never to
// nothing" invariant this file's own storage keeps by construction (above)
// stays true after an explicit reset too.
inline void set_spawn_exception_hook(spawn_exception_hook_type hook) {
  detail::spawn_exception_hook_storage =
      hook ? std::move(hook) : spawn_exception_hook_type(default_spawn_exception_hook);
}

// This thread's currently-installed spawn() exception hook.
[[nodiscard]] inline auto spawn_exception_hook() noexcept -> const spawn_exception_hook_type& {
  return detail::spawn_exception_hook_storage;
}

// Dispatches `task` as a fire-and-forget operation (issue #58): registers
// a then_fast() continuation that reports an unhandled exception through
// this thread's installed hook (above). `prio` matches then()/then_fast()'s
// own trailing, inheriting Priority parameter (issue #31/#106) - stamped
// on this same completion continuation, the one node this task's own
// dispatch actually owns.
//
// `prio` here can *only* ever reach that one completion continuation, not
// `task`'s own ongoing work - by the time a caller has a future<T> to pass
// this overload, `task`'s synchronous prefix (everything up to its own
// first co_await, since promise_type::initial_suspend() is
// std::suspend_never, est:future) has already run, at whatever priority
// was ambient at its own call site, and that priority already self-
// propagated to its first suspension point (future_awaiter<T>::
// await_suspend()'s own doc comment, est:future) before this function was
// ever entered. For a task that completes promptly, that's rarely worth
// noticing; for one that runs indefinitely (a persistent event-driven
// loop, say) it means this overload's `prio` has no observable effect at
// all - a real, easy-to-miss trap the Fn&& overload below exists
// specifically to avoid. Use that overload instead of this one whenever
// the created task's own priority - not just its unhandled-exception
// report - needs to be something other than whatever's already ambient.
//
// Pure sugar over std::move(task).then_fast(...) - no separate tracking
// of `task` beyond the continuation this registers on it (this file's own
// top comment explains why that's not needed): the continuation node's
// own owner_ reference (est:future) is what keeps future_state<T> alive
// until it completes, exactly like every other combinator in this
// codebase (with_stop(), with_timeout(), when_all(), when_any(),
// when_any_succeeds()) already relies on for the futures they register
// continuations on. then_fast() dispatches through whatever loop already
// owns `task`'s own future_state - spawn() never needs a loop& of its own
// to do this, and doesn't take one.
template <class T> void spawn(future<T> task, Priority prio = current_priority()) {
  detail::discard(std::move(task).then_fast(
      [](future<T>& f) {
        if (f.ready_with_failure()) {
          spawn_exception_hook()(f.get_exception());
        }
      },
      prio));
}

// Same, for a caller that would rather hand spawn() a callable to invoke
// than a future<T> already in flight (e.g. `est::spawn([&] { return
// some_coroutine(args...); })` when the caller wants the call itself, not
// just the await, deferred to this point) - forwards straight to the
// future<T> overload above once invoked. Constrained on std::invocable<Fn&>
// specifically (not, say, a "not a future" trait) so a future<T> argument
// unambiguously resolves to the overload above instead: future<T> has no
// operator(), so it never satisfies this constraint in the first place.
//
// Unlike the overload above, this one raises current_priority() to prio
// for the call to fn() itself (restored once it returns) - the one place
// this function can still influence the created task's own priority
// rather than just its eventual completion report. fn()'s return -
// typically a coroutine call - runs its synchronous prefix right here, on
// this call stack (the future<T> overload's own doc comment has the
// reasoning), and that prefix's first suspension point is what actually
// gets prio stamped onto it; every later co_await then inherits it in
// turn (issue #31's own priority-propagation behavior, est:loop). A
// caller that only wants prio applied to the one-time unhandled-exception
// report, not the task's own ongoing work, should pass an already-built
// future<T> to the overload above instead - constructing it themselves,
// at whatever priority is already ambient there, makes that choice
// explicit rather than incidental.
template <class Fn>
  requires(std::invocable<Fn&>)
void spawn(Fn&& fn, Priority prio = current_priority()) {
  const auto priority_guard = set_priority(prio);
  spawn(std::forward<Fn>(fn)(), prio);
}

} // namespace est
