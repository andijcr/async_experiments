export module est:sync.mutex;

import std;
import :future;
import :promise;
import :sync.event;

export namespace est {

// A cooperative-scheduling mutex: awaitable lock(), guarding a critical
// section that spans a coroutine suspension point rather than a plain
// synchronous one. There is still only ever one thread of control in
// this single-threaded framework, but a *cooperative* race is real here:
// two coroutines each doing "read some shared structure, co_await
// something, write it back" can still interleave at that suspension
// point and corrupt it, exactly the way two OS threads would without a
// lock. `co_await mutex.lock();` is this framework's answer to that.
//
// Built directly on est::binary_event<EventResetMode::automatic>
// (est:sync.event) rather than its own bespoke waiter-list/resume-node
// pair (issue #67): "unlocked" is exactly "one unit available," "locked"
// is "no unit available," lock() is wait(), and unlock() is set() -
// automatic mode's own "hand the unit directly to the next queued
// waiter, never reading as free in between" behavior (counting_event<Mode>
// ::set()'s own doc comment, est:sync.event) is already exactly the
// handoff semantics a mutex needs. No remaining code here re-implements
// any part of the waiter queue, the resume node, or the abandoned-waiter
// exception-completion path - event_'s own destructor/wait()/set()
// already are that code.
//
// lock() returns `future<lock_guard>` directly - there is no separate
// lock()-returns-future<void> plus acquire()-returns-future<lock_guard>
// pair any more (issue #66): a lock without a guard to release it has no
// use in this codebase (every call site immediately wants the RAII
// handle), so there is nothing a second, lower-level primitive would add
// besides a way to forget to unlock(). unlock() itself is private,
// reachable only through lock_guard's own destructor/move-assignment (a
// nested class already shares its enclosing class's access) - the only
// way to release the lock this class exposes at all now.
class mutex {
public:
  // event_ starts at max_count (1) but count() 0 (not yet signaled, see
  // counting_event<Mode>'s own doc comment) - one set() here brings it to
  // 1, "available," matching an unlocked mutex's own starting state.
  // Never resolves current_loop(): set() only does that once something is
  // actually queued in waiters_, never true for a mutex that was just
  // constructed.
  mutex() noexcept { event_.set(); }

  mutex(const mutex&) = delete;
  auto operator=(const mutex&) -> mutex& = delete;
  mutex(mutex&&) = delete;
  auto operator=(mutex&&) -> mutex& = delete;
  ~mutex() = default;

  [[nodiscard]] auto locked() const noexcept -> bool { return event_.count() == 0; }

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return event_.has_waiters(); }

  class lock_guard;

  // Acquires the lock, resolving once it's held - suspending the calling
  // coroutine first (if `co_await`ed) when it's already held. The fast
  // path (uncontended) consumes the available unit synchronously via
  // try_wait() and returns an already-ready future, with no future/
  // node allocated beyond the returned future_state<lock_guard> itself -
  // matching this class's previous zero-extra-allocation fast path
  // exactly (see [Allocation Patterns](../wiki/Allocation-Patterns.md)).
  // The slow path defers through event_.wait().then(...) instead: once
  // that future<void> resolves (event_'s own waiter mechanism, unlock()
  // handing the unit directly to the next queued waiter), the attached
  // continuation builds the lock_guard and completes the future<lock_guard>
  // this method returned. Costs two more allocations than the old
  // hand-written acquire_resume_node did (a concrete_continuation<Fn,
  // lock_guard> node plus its own downstream future_state<lock_guard>, on
  // top of event_.wait()'s own future_state<void>+detail::promise_resume_node<void>) - a
  // deliberate trade of contended-path allocation count for not
  // re-implementing the waiter queue/resume node/abandonment-completion
  // machinery a second time; see docs/PLAN.md's "Issue #66 & #67" entry.
  //
  // The `.then()` callback captures `this` (a `mutex*`) - the exact same
  // "needs the mutex still alive once the deferred completion actually
  // runs" precondition the old acquire_resume_node::run() carried, not a
  // new hazard this refactor introduced; see issue #46 (still open,
  // unaffected by this change) and docs/wiki/Coroutines.md's own section
  // on it.
  //
  // Defined out-of-line, below lock_guard - needs to be a complete type
  // first (future<lock_guard>/make_ready_future<lock_guard>/.then() all
  // need it complete, not just forward-declared as it is here).
  [[nodiscard]] auto lock() -> future<lock_guard>;

private:
  // Releases the lock: event_.set() hands the freed unit directly to the
  // next queued waiter, if any (never reading as free in between - see
  // this class's own top comment), or leaves it available for the next
  // lock() otherwise. Private - lock_guard (a nested class, sharing this
  // class's access) is the only caller, so the only way to release a
  // lock acquired through this class is to drop the guard lock()
  // returned.
  void unlock() noexcept { event_.set(); }

  binary_event<EventResetMode::automatic> event_;
};

// RAII handle returned by mutex::lock(): the mutex is held for as long as
// one of these is alive, unlocked automatically from the destructor - no
// separate unlock() call for a caller to remember (issue #66 - there is
// no lower-level manual pair any more, unlock() itself is private).
// Move-only, matching every other single-owner handle in this codebase
// (future<T>, promise<T>): copying would let two guards each believe they
// owned unlocking the same mutex, double-unlocking it once both were
// destroyed.
class mutex::lock_guard {
public:
  explicit lock_guard(mutex& mutex_ref) noexcept : mutex_(&mutex_ref) {}
  lock_guard(const lock_guard&) = delete;
  auto operator=(const lock_guard&) -> lock_guard& = delete;

  // mutex_ set to nullptr on the moved-from side so its own destructor
  // becomes a no-op - the same "empty after move" contract shared_ptr<T>
  // (est:util.shared_ptr) already documents for exactly this reason.
  lock_guard(lock_guard&& other) noexcept : mutex_(std::exchange(other.mutex_, nullptr)) {}
  auto operator=(lock_guard&& other) noexcept -> lock_guard& {
    if (this != &other) {
      if (mutex_ != nullptr) {
        mutex_->unlock();
      }
      mutex_ = std::exchange(other.mutex_, nullptr);
    }
    return *this;
  }

  ~lock_guard() {
    if (mutex_ != nullptr) {
      mutex_->unlock();
    }
  }

private:
  mutex* mutex_;
};

inline auto mutex::lock() -> future<lock_guard> {
  if (event_.try_wait()) {
    return make_ready_future<lock_guard>(*this);
  }
  return event_.wait().then([this] { return lock_guard(*this); });
}

} // namespace est
