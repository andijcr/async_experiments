export module est:spawn;

import std;
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
// Not built on issue #103's central, id-keyed waiter registry - that issue
// is still a design pass with no implementation and no decision yet among
// its own four candidate shapes, not something to block this on. What
// spawn() needs is narrower than what #103 is solving anyway: #103 is
// about retrofitting eager, external cancellation into *existing*, shared
// waiter lists (mutex, counting_event, future_state<T>'s own waiters_).
// spawn() just needs a *new*, loop-owned collection of in-flight tasks,
// each removed by its own completion continuation - no external
// cancellation, no aliasing hazard, none of #103's harder cases
// (generational safety, pointer tagging). That's the exact shape
// loop::pending_timers_ already uses (#103's own "option A": a synthetic
// registry + linear scan on removal) - detail::spawned_entry/
// loop::track_spawned()/untrack_spawned() (est:loop) mirror it directly,
// scoped to spawn() alone rather than generalized to every waiter list.
// Worth reconciling with #103's registry later if that ever lands; not
// worth waiting for it now.

namespace est::detail {

// The concrete counterpart to detail::spawned_entry (est:loop): :loop
// itself never names future<T> (that file's own top comment), so the
// type-erased base lives there and this - the only thing that actually
// needs to know T - lives here instead. Purely a keep-alive: holding
// future_ here is what gives the spawned task's future_state<T> an
// explicit, loop-owned reason to stay alive, independent of whatever else
// might (or might not) reference it - see est::spawn()'s own doc comment
// below for the full reasoning. current_allocator_new_delete<spawn_entry<T>>
// (est:util.current_loop), not a bare `new`: the same allocator-aware
// operator new/delete every other node type tracked via a unique_ptr in an
// est::loop-owned collection already uses (est:loop's own ready_node/
// timer_node doc comment has the full reasoning for why that mixin exists
// instead of living on the type-erased base itself).
template <class T>
class spawn_entry final : public spawned_entry,
                          public current_allocator_new_delete<spawn_entry<T>> {
public:
  explicit spawn_entry(future<T> task) noexcept : future_(std::move(task)) {}

  [[nodiscard]] auto task() noexcept -> future<T>& { return future_; }

private:
  future<T> future_;
};

} // namespace est::detail

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

// Dispatches `task` as an explicitly loop-owned fire-and-forget operation
// (issue #58): registers a then_fast() continuation that observes the
// result, reports an unhandled exception through loop's own installed
// hook, and reclaims the tracking entry loop::track_spawned() (est:loop)
// created for it - giving `task`'s future_state<T> an explicit,
// unambiguous reason to stay alive until it completes rather than the
// previous, implicit "stays alive because a continuation happens to
// reference it" convention every call site had to explain on its own.
// `prio` matches then()/then_fast()'s own trailing, inheriting Priority
// parameter (issue #31/#106) - stamped on this same completion
// continuation, the one node this task's own dispatch actually owns.
//
// Self-heals loop's hook to the default before ever reading it, if
// nothing has installed one yet (a loop nobody has called
// set_spawn_exception_hook() on at all) - together with
// set_spawn_exception_hook() above always installing a real value instead
// of forwarding an empty one through, this keeps the completion
// continuation below able to call the hook unconditionally, no
// empty-vs-installed branch needed at the one place that actually reads
// it every time a spawned task completes.
//
// entry->task().then_fast(...), not std::move(task).then_fast(...)
// directly: the continuation has to be registered on the *same* future<T>
// handle spawn_entry<T> goes on to store (loop::track_spawned() below
// takes ownership of `task` first), not a second, independent one - a
// non-scalar T's future<T> can't be clone()d (future<T>::clone(), est:
// future), so there is only ever one handle to register on. Calling
// then_fast() on that stored handle as an lvalue (its & overload,
// future.cppm) registers the continuation without consuming the handle,
// leaving spawn_entry<T> holding it for as long as the task is tracked.
template <class T> void spawn(loop& loop_ref, future<T> task, Priority prio = current_priority()) {
  if (!loop_ref.spawn_exception_hook()) {
    set_spawn_exception_hook(loop_ref, nullptr);
  }
  auto* entry = static_cast<detail::spawn_entry<T>*>(
      loop_ref.track_spawned(std::make_unique<detail::spawn_entry<T>>(std::move(task))));
  detail::discard(entry->task().then_fast(
      [&loop_ref, entry](future<T>& f) {
        if (f.ready_with_failure()) {
          loop_ref.spawn_exception_hook()(f.get_exception());
        }
        loop_ref.untrack_spawned(entry);
      },
      prio));
}

// Same, for a caller that would rather hand spawn() a callable to invoke
// than a future<T> already in flight (e.g. `est::spawn(loop, [&] { return
// some_coroutine(args...); })` when the caller wants the call itself, not
// just the await, deferred to this point) - forwards straight to the
// future<T> overload above once invoked. Constrained on std::invocable<Fn&>
// specifically (not, say, a "not a future" trait) so a future<T> argument
// unambiguously resolves to the overload above instead: future<T> has no
// operator(), so it never satisfies this constraint in the first place.
template <class Fn>
  requires(std::invocable<Fn&>)
void spawn(loop& loop_ref, Fn&& fn, Priority prio = current_priority()) {
  spawn(loop_ref, std::forward<Fn>(fn)(), prio);
}

} // namespace est
