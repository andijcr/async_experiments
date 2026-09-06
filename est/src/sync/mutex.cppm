export module est:sync.mutex;

import std;
import :future;
import :loop;
import :promise;
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
// the awaiter, below. acquire() (below) packages the same lock()/unlock()
// pair as a future<lock_guard> instead, for a caller that would rather
// have the lock released automatically (RAII) than remember to call
// unlock() itself.
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

  // Destroys (without resuming) any waiter still queued on lock() or
  // acquire() - mirrors future_state<T>::~future_state() and loop::~loop()
  // (both drain their own pending lists the same way, for the same
  // reason). Without this, a waiter still queued in waiters_ when *this
  // is destroyed would simply be unreachable: a lock()-registered
  // lock_resume_node would leak its coroutine's frame, never resumed *or*
  // destroyed (found in review - an earlier version of this destructor
  // was simply `= default`); an acquire()-registered acquire_resume_node
  // would leak the node itself and permanently strand its future<lock_guard>
  // not-ready.
  ~mutex() {
    waiters_.drain([this](detail::ready_node& node) { node.destroy(loop_.allocator()); });
  }

  [[nodiscard]] auto locked() const noexcept -> bool { return state_ != 0; }

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return !waiters_.empty(); }

  class lock_awaiter;
  class lock_resume_node;
  class lock_guard;
  class acquire_resume_node;

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

  // acquire() is lock()/unlock() packaged as a future<lock_guard> instead
  // of a co_await-only primitive (repo owner's own framing, PR #37 review:
  // "keep lock/unlock void, add acquire() -> future<lock_guard>, lock_guard
  // is move only") - a caller that doesn't want to remember to call
  // unlock() itself gets a RAII handle instead, exactly like every other
  // guard type in this codebase's own ecosystem (est::scope_exit). Not a
  // coroutine itself - see acquire_resume_node's own doc comment for why
  // it's built directly on est::promise<lock_guard>, the same "producer
  // built without co_await" pattern sleep_until() (est:promise) already
  // uses on top of est::loop's timer queue.
  //
  // Mirrors lock_awaiter::await_ready()'s own fast path exactly: an
  // unlocked mutex is acquired immediately and returns an already-ready
  // future, no different from any other producer that calls set_value()
  // before ever handing back its future (e.g. make_promise_future() +
  // an immediate set_value()) - this is not the "run a registered
  // continuation inline" hazard this codebase otherwise forbids (M3,
  // docs/PLAN.md), since nothing is registered against this future yet.
  // Defined out-of-line, below lock_guard/acquire_resume_node - both
  // need to be complete types first.
  [[nodiscard]] auto acquire() -> future<lock_guard>;

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

// RAII handle returned by mutex::acquire(): the mutex is held for as long
// as one of these is alive, unlocked automatically from the destructor -
// no separate unlock() call for a caller to remember, unlike lock()/
// unlock() themselves (deliberately left as the lower-level, manual
// primitive - repo owner's own call, PR #37 review). Move-only, matching
// every other single-owner handle in this codebase (future<T>, promise<T>):
// copying would let two guards each believe they owned unlocking the same
// mutex, double-unlocking it once both were destroyed.
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

// The waiter node behind acquire()'s slow path (mutex already locked):
// queued in mutex::waiters_ exactly like lock_resume_node, but completes a
// est::promise<lock_guard> instead of resuming a coroutine handle - no
// coroutine frame is involved on this path at all, so there's no
// "abandoned, still fully intact" case to guard against the way
// lock_resume_node's ran_ flag does: promise_'s own destructor already
// handles being dropped without ever completing correctly (a "broken
// promise" - the future simply never becomes ready - the same contract
// every other est::promise<T> in this codebase already has), so destroy()
// here is a plain deallocation, nothing more.
class mutex::acquire_resume_node final : public detail::ready_node {
public:
  acquire_resume_node(mutex& mutex_ref, promise<lock_guard> prom) noexcept
      : mutex_(mutex_ref), promise_(std::move(prom)) {}

  // Called only once unlock() has already handed this waiter the lock
  // (mutex::unlock() hands off rather than clearing state_ - see its own
  // doc comment), so constructing the lock_guard here doesn't need to
  // touch state_ itself at all.
  void run() final { promise_.set_value(lock_guard(mutex_)); }

  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept final {
    allocator.delete_object(this);
  }

private:
  mutex& mutex_;
  promise<lock_guard> promise_;
};

inline auto mutex::acquire() -> future<lock_guard> {
  auto [prom, fut] = make_promise_future<lock_guard>(loop_);
  if (state_ == 0) {
    state_ = 1;
    prom.set_value(lock_guard(*this));
    return std::move(fut);
  }
  auto* node = loop_.allocator().template new_object<acquire_resume_node>(*this, std::move(prom));
  waiters_.enqueue(*node);
  return std::move(fut);
}

} // namespace est
