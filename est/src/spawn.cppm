export module est:spawn;

import std;
import :check;
import :future;
import :loop;
import :platform;
import :sync.stop_token;
import :util.current_loop;

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
// spawn() also no longer takes an explicit loop& (PR #122 review): it
// resolves est::current_loop() (:util.current_loop) instead, the same
// ambient mechanism make_promise_future()/sleep_for()/sleep_until()/
// yield_execution()/est::mutex/est::counting_event<Mode> already resolve
// through. A caller registers a loop once via
// est::make_current_loop_with_spawn() (below) - a thin wrapper around
// est::make_current_loop() (:util.current_loop) that also installs the
// default exception hook if nothing has set one yet. Doing this once, at
// registration time, replaces an earlier version of spawn() that
// self-healed a still-empty hook lazily on every call.

export namespace est {

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

// The preferred way to install (or reset) loop's spawn() exception hook -
// prefer this over calling loop::set_spawn_exception_hook() (est:loop)
// directly. Keeps the invariant spawn() itself (below) relies on: once
// anything has touched loop's hook through this entry point, it's never
// empty again. Passing an empty/null `hook` explicitly resets to
// default_spawn_exception_hook() above, rather than forwarding the empty
// value through and leaving loop with nothing installed -
// loop::set_spawn_exception_hook() itself can't provide this guarantee on
// its own, since :loop has no way to name default_spawn_exception_hook
// (this file's own top comment on why :loop can't import :spawn).
inline void set_spawn_exception_hook(loop& loop_ref, loop::exception_hook_type hook) {
  loop_ref.set_spawn_exception_hook(hook ? std::move(hook)
                                         : loop::exception_hook_type(default_spawn_exception_hook));
}

// Registers loop_ref as the current loop (est::make_current_loop(),
// :util.current_loop) and makes sure it has a spawn exception hook
// installed - the default one, unless something has already set a
// different one via set_spawn_exception_hook() above. Doing this once,
// here, at registration time is what lets spawn() below read
// loop_ref.spawn_exception_hook() unconditionally instead of re-checking
// on every call - the preferred way to bring up a loop for any program
// that uses est::spawn(). Plain est::make_current_loop() still works for
// a loop that never calls spawn() at all (every other current_loop()-based
// entry point - make_promise_future(), sleep_for(), est::mutex, ... -
// doesn't touch this hook either way).
[[nodiscard]] auto make_current_loop_with_spawn(loop& loop_ref) {
  if (!loop_ref.spawn_exception_hook()) {
    set_spawn_exception_hook(loop_ref, nullptr);
  }
  return make_current_loop(loop_ref);
}

// Dispatches `task` as a fire-and-forget operation (issue #58): registers
// a then_fast() continuation that reports an unhandled exception through
// current_loop()'s own installed hook. `prio` matches then()/then_fast()'s
// own trailing, inheriting Priority parameter (issue #31/#106) - stamped
// on this same completion continuation, the one node this task's own
// dispatch actually owns.
//
// Pure sugar over std::move(task).then_fast(...) - no separate tracking
// of `task` beyond the continuation this registers on it (this file's own
// top comment explains why that's not needed): the continuation node's
// own owner_ reference (est:future) is what keeps future_state<T> alive
// until it completes, exactly like every other combinator in this
// codebase (with_stop(), with_timeout(), when_all(), when_any(),
// when_any_succeeds()) already relies on for the futures they register
// continuations on.
//
// Resolves current_loop() rather than taking a loop& parameter, matching
// every other ambient-aware entry point here (make_promise_future(),
// sleep_for()/sleep_until(), est::mutex, est::counting_event<Mode>) -
// register the loop first via est::make_current_loop_with_spawn() above.
// The check() below is this function's own precondition, distinct from
// current_loop()'s: it catches a loop registered via plain
// est::make_current_loop() instead of the spawn-aware wrapper, which
// would otherwise leave the hook empty and turn a genuine unhandled
// exception into an opaque std::bad_function_call instead of a clear
// diagnostic naming the fix.
template <class T> void spawn(future<T> task, Priority prio = current_priority()) {
  loop& loop_ref = current_loop();
  detail::discard(std::move(task).then_fast(
      [&loop_ref](future<T>& f) {
        if (f.ready_with_failure()) {
          check(static_cast<bool>(loop_ref.spawn_exception_hook()),
                "est::spawn(): the current loop has no exception hook installed - "
                "register it via est::make_current_loop_with_spawn() instead of "
                "est::make_current_loop()");
          loop_ref.spawn_exception_hook()(f.get_exception());
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
template <class Fn>
  requires(std::invocable<Fn&>)
void spawn(Fn&& fn, Priority prio = current_priority()) {
  spawn(std::forward<Fn>(fn)(), prio);
}

} // namespace est
