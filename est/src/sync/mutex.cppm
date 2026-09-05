export module est:sync.mutex;

import std;
import :loop;
import :util.intrusive_list;

export namespace est {

// A cooperative-scheduling mutex: awaitable lock(), guarding a critical
// section that spans a coroutine suspension point rather than a plain
// synchronous one. There is still only ever one thread of control in
// this single-threaded framework - the interrupt-context reentrancy
// est::mutex was originally built to guard against was already removed
// as premature back in M1 (docs/PLAN.md) - but a *cooperative* race is
// real here: two coroutines each doing "read some shared structure,
// co_await something, write it back" can still interleave at that
// suspension point and corrupt it, exactly the way two OS threads would
// without a lock. `co_await mutex.lock();` is this framework's answer to
// that, built entirely on primitives this codebase already has -
// est::detail::ready_node (est:loop) for the waiter/resume queue,
// est::intrusive_list for the queue itself - needing nothing new besides
// the awaiter, below.
//
// Holds a `loop&` (M3's convention, same as future_state<T> - see that
// class's own doc comment) rather than its own allocator: it needs
// somewhere to enqueue a waiter's resumption once unlock() hands the
// lock to it, and est::loop::enqueue_ready() is that somewhere. Same
// lifetime precondition as every other loop-holding type in this
// codebase: the loop must outlive every mutex constructed against it.
class mutex {
public:
  explicit mutex(loop& loop_ref) noexcept : loop_(loop_ref) {}
  mutex(const mutex&) = delete;
  auto operator=(const mutex&) -> mutex& = delete;
  mutex(mutex&&) = delete;
  auto operator=(mutex&&) -> mutex& = delete;

  // Destroys (without resuming) any coroutine still waiting on lock() -
  // mirrors future_state<T>::~future_state() and loop::~loop() (both
  // drain their own pending lists the same way, for the same reason).
  // Without this, a coroutine still queued in waiters_ when *this is
  // destroyed would simply be unreachable - its lock_resume_node leaked,
  // and the coroutine itself never resumed *or* destroyed, permanently
  // hung and leaking its own frame (found in review - an earlier version
  // of this destructor was simply `= default`).
  ~mutex() {
    waiters_.drain([this](detail::ready_node& node) { node.destroy(loop_.allocator()); });
  }

  [[nodiscard]] auto locked() const noexcept -> bool { return state_ != 0; }

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return !waiters_.empty(); }

  class lock_awaiter;
  class lock_resume_node;

  // Returns an awaiter: `co_await mutex.lock();` acquires the lock,
  // suspending the calling coroutine first if it's already held. Only
  // meaningful inside a coroutine - see lock_awaiter's own doc comment.
  [[nodiscard]] auto lock() noexcept -> lock_awaiter;

  // Releases the lock. If a coroutine is waiting, hands the lock
  // directly to it instead of clearing state_ - the resumed waiter
  // becomes the new holder without any window where the lock reads as
  // free, so a third party couldn't legally lock() in between even in a
  // hypothetical multi-loop future. Resumption is deferred through
  // est::loop::enqueue_ready() rather than an inline handle.resume()
  // call here, the same "never inline, always deferred through the
  // loop" invariant every other producer in this codebase keeps
  // (future_state<T>::complete(), a fired timer) - unlock() calling
  // resume() synchronously would otherwise run the next waiter's entire
  // remaining coroutine body nested inside this call, unbounded in depth
  // for a chain of coroutines that each lock/unlock in turn.
  void unlock() noexcept {
    if (auto* waiter = waiters_.dequeue()) {
      loop_.enqueue_ready(*waiter);
      return;
    }
    state_ = 0;
  }

private:
  friend class lock_awaiter;

  int state_ = 0;
  loop& loop_;
  intrusive_list<detail::ready_node> waiters_;
};

// A plain resumption trampoline, queued in mutex::waiters_ in place of
// the awaiter itself: unlock() hands it straight to
// est::loop::enqueue_ready() with no extra wrapping - run() resumes the
// coroutine waiting on it. Separately heap-allocated via the mutex's own
// loop's allocator, *not* embedded in the awaiting coroutine's frame the
// way an earlier version of this design tried - est::loop::run_one()
// calls destroy() on this same node well after run() already resumed
// that coroutine past its own suspension point, and a frame-embedded
// node's storage may by then already be reused for whatever the
// coroutine's later code constructs, making that destroy() call
// undefined behavior (confirmed the hard way: libc++abi's "Pure virtual
// function called!", a dispatch through an already-clobbered vtable
// pointer). See est:future's own detail::coroutine_resume_node for the
// identical reasoning and fix, applied there first.
class mutex::lock_resume_node final : public detail::ready_node {
public:
  explicit lock_resume_node(std::coroutine_handle<> handle) noexcept : handle_(handle) {}

  void run() final {
    ran_ = true;
    handle_.resume();
  }

  // See est:future's own detail::coroutine_resume_node::destroy() for the
  // full "why check ran_ before touching handle_" reasoning - identical
  // here: if run() never happened (this node was still queued in
  // mutex::waiters_ when the mutex itself was destroyed), the waiting
  // coroutine is still fully intact and untouched, so this is the only
  // chance to free its frame; if run() did happen, the coroutine either
  // already self-destroyed or suspended again on something that now owns
  // it, and touching handle_ again here would be wrong either way.
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept final {
    if (!ran_) {
      handle_.destroy();
    }
    allocator.delete_object(this);
  }

private:
  std::coroutine_handle<> handle_;
  bool ran_ = false;
};

// Awaiter returned by mutex::lock(). *this is a coroutine-frame subobject
// (like est:future's own coroutine_start_awaiter/future_awaiter<T>) -
// fine here for the identical reason: await_suspend() below allocates
// the independently-lived lock_resume_node before this awaiter's own
// co_await expression ends, and nothing reaches back into *this
// afterward.
class mutex::lock_awaiter final {
public:
  explicit lock_awaiter(mutex& mutex_ref) noexcept : mutex_(mutex_ref) {}
  // Deleted, not defaulted - never actually invoked: mutex::lock()
  // returns one as a genuine prvalue, which C++17's mandatory copy
  // elision builds in place with no copy/move at all (the same
  // guaranteed-elision pattern est::scope_exit's own doc comment -
  // est:util.scope_exit - already documents relying on).
  lock_awaiter(const lock_awaiter&) = delete;
  auto operator=(const lock_awaiter&) -> lock_awaiter& = delete;
  lock_awaiter(lock_awaiter&&) = delete;
  auto operator=(lock_awaiter&&) -> lock_awaiter& = delete;
  ~lock_awaiter() = default;

  // The fast path: an unlocked mutex is acquired immediately, with no
  // suspension at all - await_suspend() below never runs in this case.
  [[nodiscard]] auto await_ready() noexcept -> bool {
    if (mutex_.locked()) {
      return false;
    }
    mutex_.state_ = 1;
    return true;
  }

  // The slow path: already locked, so a freshly allocated
  // lock_resume_node joins the waiter queue in *this awaiter's place,
  // and *this genuinely suspends. unlock() resumes that node later,
  // handing over the lock rather than clearing state_ first - state_
  // simply stays 1 the whole time, since ownership passes directly from
  // the unlocking coroutine to this one.
  void await_suspend(std::coroutine_handle<> handle) {
    auto* node = mutex_.loop_.allocator().new_object<lock_resume_node>(handle);
    mutex_.waiters_.enqueue(*node);
  }

  // Nothing to return - by the time this runs, the lock is held either
  // way (the fast path set state_ itself; the slow path only resumes
  // once unlock() has already handed the lock to the queued
  // lock_resume_node).
  void await_resume() const noexcept {}

private:
  mutex& mutex_;
};

inline auto mutex::lock() noexcept -> lock_awaiter {
  return lock_awaiter(*this);
}

} // namespace est
