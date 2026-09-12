export module est:sync.mutex;

import std;
import :future;
import :loop;
import :promise;
import :util.current_loop;
import :util.intrusive_list;

export namespace est {

// A cooperative-scheduling mutex: awaitable lock(), guarding a critical
// section that spans a coroutine suspension point rather than a plain
// synchronous one. There is still only ever one thread of control in
// this single-threaded framework, but a *cooperative* race is real here:
// two coroutines each doing "read some shared structure, co_await
// something, write it back" can still interleave at that suspension
// point and corrupt it, exactly the way two OS threads would without a
// lock. `co_await mutex.lock();` is this framework's answer to that,
// built entirely on primitives this codebase already has -
// est::detail::ready_node (est:loop) for the waiter/resume queue,
// est::intrusive_list for the queue itself, est::promise<T>/est::future<T>
// for the result itself. acquire() packages the same lock()/unlock()
// pair as a future<lock_guard> instead, for a caller that would rather
// have the lock released automatically (RAII) than remember to call
// unlock() itself.
//
// lock() returns a plain `future<void>`, built the same way `acquire()`
// is - no bespoke awaiter type: `co_await mutex.lock()` on an
// uncontended mutex resumes immediately, with no extra node allocated,
// through the same path any other `co_await`-of-an-already-ready-future
// takes (`future_awaiter<T>::await_ready()`, `est:future`).
//
// Holds no `loop&` of its own, unlike future_state<T> - every method
// below resolves est::current_loop()/est::current_allocator() fresh, at
// the point of use, rather than caching anything at construction time -
// only current_allocator(), not current_loop() itself, where a method
// never actually needs the loop (see lock()/acquire()/~mutex()'s own
// comments). There is no constructor taking an explicit `loop&` either;
// a mutex is never tied to a specific loop, only to whichever one is
// current when each operation runs.
class mutex {
public:
  mutex() noexcept = default;

  mutex(const mutex&) = delete;
  auto operator=(const mutex&) -> mutex& = delete;
  mutex(mutex&&) = delete;
  auto operator=(mutex&&) -> mutex& = delete;

  // Destroys (without granting the lock) any waiter still queued on
  // lock() or acquire() - mirrors future_state<T>::~future_state() and
  // loop::~loop() (both drain their own pending lists the same way, for
  // the same reason). Each waiter node's own abandon() (below) completes
  // its promise with an exception first, rather than just deallocating
  // itself silently - see lock_resume_node's own doc comment for why
  // that matters beyond just freeing the node itself.
  // waiters_.empty() checked first - a mutex with nothing queued can
  // legitimately be destroyed long after whatever loop was current when
  // it was created has stopped being current at all, and deleting an
  // abandoned waiter node resolves est::current_allocator() fresh,
  // inside that node's own operator delete (est::detail::ready_node's
  // own doc comment, est:loop), which would fail its own precondition in
  // that case even though nothing here actually needs one. current_loop()
  // itself is never resolved here at all.
  ~mutex() {
    if (waiters_.empty()) {
      return;
    }
    waiters_.drain(detail::abandon_ready_node);
  }

  [[nodiscard]] auto locked() const noexcept -> bool { return state_ != 0; }

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return !waiters_.empty(); }

  class lock_resume_node;
  class lock_guard;
  class acquire_resume_node;

  // Acquires the lock, resolving once it's held - suspending the calling
  // coroutine first (if `co_await`ed) when it's already held. Built
  // directly on est::promise<void>/est::future<void>, not a coroutine of
  // its own or a bespoke awaiter type - see this class's own doc comment
  // for why. The fast path (uncontended) completes the promise
  // immediately and returns an already-ready future; the slow path
  // queues a lock_resume_node in waiters_, completed later by unlock().
  // Defined out-of-line, below lock_resume_node - needs to be a complete
  // type first.
  [[nodiscard]] auto lock() -> future<void>;

  // Releases the lock. If a waiter is queued, hands the lock directly to
  // it instead of clearing state_ - the resumed waiter becomes the new
  // holder without any window where the lock reads as free, so a third
  // party couldn't legally lock() in between even in a hypothetical
  // multi-loop future.
  //
  // Deferred through est::loop::enqueue_ready() rather than completed
  // directly here: completing synchronously would run() the waiter
  // unconditionally, even one that's a coroutine later abandoned
  // (destroyed without ever consuming its future<lock_guard>).
  // Abandonment is only safe because abandon() (never run()) completes
  // with an exception instead of a real value - see lock_resume_node's
  // own doc comment. Deferring through the loop keeps that guard rail
  // intact: whether a queued waiter is later run() (the loop drains it
  // normally) or abandon()ed and deleted (the loop or this mutex is torn
  // down first) is decided at drain time, not decided in advance here.
  //
  // KNOWN LIMITATION left open by keeping this deferred:
  // acquire_resume_node::run() still needs a live mutex& to construct the
  // lock_guard it completes with. If this mutex is destroyed after
  // unlock() enqueues a waiter but before the loop actually drains it -
  // only possible if the loop outlives this mutex and keeps running
  // during that gap, since ~mutex() itself drains waiters_ via abandon()/
  // delete, never run() - that waiter's run() would construct a
  // lock_guard over an already-dangling mutex&. Left as a documented
  // precondition: the
  // loop must outlive every mutex constructed against it, and a caller
  // must not let the loop keep running past a mutex's destruction while
  // a waiter is still queued on it.
  //
  // SECOND KNOWN LIMITATION, from not caching a loop& (this class's own
  // doc comment): a waiter enqueued by lock()/acquire() carries an
  // allocator and a future_state<T> built against whichever loop was
  // current *then*. unlock() (and ~mutex(), above) instead resolve
  // est::current_loop() fresh, *now*. If the current loop has changed
  // in between - a caller nesting a second make_current_loop() scope
  // around some unrelated work, say - the waiter is handed to, or
  // destroyed through the allocator of, a different loop than the one
  // its own future_state actually belongs to: at best a future that
  // never resolves on the loop actually being run; at worst deallocating
  // through the wrong memory_resource. A cached `loop&` member (what
  // this class had before) made this hazard structurally impossible -
  // every operation on one mutex used the one loop it was built against,
  // full stop. Resolving current_loop() fresh does not.
  void unlock() noexcept {
    if (auto* waiter = waiters_.dequeue()) {
      current_loop().enqueue_ready(*waiter);
      return;
    }
    state_ = 0;
  }

  // acquire() is lock()/unlock() packaged as a future<lock_guard> instead
  // of a manual pair - a caller that doesn't want to remember to call
  // unlock() itself gets a RAII handle instead, exactly like every other
  // guard type in this codebase (est::scope_exit). Built the same way
  // lock() is - see acquire_resume_node's own doc comment for the one
  // place its shape differs from lock_resume_node's.
  //
  // Defined out-of-line, below lock_guard/acquire_resume_node - both need
  // to be complete types first.
  [[nodiscard]] auto acquire() -> future<lock_guard>;

private:
  int state_ = 0;
  intrusive_list<detail::ready_node> waiters_;
};

// The waiter node behind lock()'s slow path (mutex already locked):
// queued in mutex::waiters_, completing a est::promise<void> once
// unlock() hands it the lock.
//
// abandon() completing the promise with an exception (rather than simply
// deallocating the node) matters specifically for the case where run()
// never happened: this node was still sitting in mutex::waiters_ when
// the mutex itself was torn down, never handed the lock. Silently
// dropping the promise instead (the "broken promise, future simply never
// becomes ready" contract every other est::promise<T> in this codebase
// otherwise has) would leave a coroutine suspended on the resulting
// future<void> via co_await stranded forever: its pending
// future_resume_node<T> (est:future) sits inside this future_state's own
// waiters_, reachable only once the future_state completes, and nothing
// else holds a reference to free the coroutine's frame. Completing the
// promise here drains the future_state's own pending continuation onto
// est::loop's ready_ queue instead, where it's either genuinely resumed
// (if the loop outlives this mutex and keeps running) or safely
// destroyed, never run, by est::loop's own destructor-time drain
// (loop::~loop()). abandon() (est::detail::ready_node's own doc comment,
// est:loop) is exactly what stops this from double-completing an
// already-successfully-completed promise: it's never called on a node
// the loop already drained via `run()` (unlock() handed it the lock) -
// only on one still sitting in `waiters_` or still queued on the loop's
// own ready_ list when `~mutex()`'s or `~loop()`'s own drain reaches it
// without `run()` ever having been called.
class mutex::lock_resume_node final
    : public detail::ready_node,
      public detail::current_allocator_new_delete<lock_resume_node> {
public:
  explicit lock_resume_node(promise<void> prom) noexcept : promise_(std::move(prom)) {}

  void run() final { promise_.set_value(); }

  void abandon() noexcept final {
    promise_.set_exception(
        std::make_exception_ptr(std::runtime_error("mutex destroyed while lock() was pending")));
  }

  // operator new/delete inherited from detail::current_allocator_new_delete<T>
  // (est:util.current_loop) - see that class's own doc comment for why
  // every concrete ready_node/timer_node needs its own pair rather than
  // one shared at the ready_node base itself.

private:
  promise<void> promise_;
};

// Only current_allocator() is needed here, never current_loop() itself -
// the fast path completes the promise inline (make_ready_future()), and
// the slow path only enqueues into this mutex's own waiters_, not onto
// any loop's ready-queue (that happens later, from unlock()).
inline auto mutex::lock() -> future<void> {
  if (state_ == 0) {
    state_ = 1;
    return make_ready_future<void>();
  }
  auto [prom, fut] = detail::make_promise_future_impl<void>(current_allocator());
  auto* node = new lock_resume_node(std::move(prom));
  waiters_.enqueue(*node);
  return std::move(fut);
}

// RAII handle returned by mutex::acquire(): the mutex is held for as long
// as one of these is alive, unlocked automatically from the destructor -
// no separate unlock() call for a caller to remember, unlike lock()/
// unlock() themselves (the lower-level, manual primitive). Move-only,
// matching every other single-owner handle in this codebase (future<T>,
// promise<T>): copying would let two guards each believe they owned
// unlocking the same mutex, double-unlocking it once both were
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

// The waiter node behind acquire()'s slow path (mutex already locked):
// queued in mutex::waiters_ exactly like lock_resume_node, completing a
// est::promise<lock_guard> instead of a est::promise<void> - the only
// real difference between the two. Needs a mutex& (lock_resume_node
// doesn't) purely to construct that lock_guard once run() knows unlock()
// has actually handed it the lock; see lock_resume_node's own doc
// comment for why abandon() needs the same exception-completion on the
// abandoned (never handed the lock) path. mutex_ staying valid for
// run()'s use relies on the precondition unlock()'s own doc comment
// documents as a known, open limitation.
class mutex::acquire_resume_node final
    : public detail::ready_node,
      public detail::current_allocator_new_delete<acquire_resume_node> {
public:
  acquire_resume_node(mutex& mutex_ref, promise<lock_guard> prom) noexcept
      : mutex_(mutex_ref), promise_(std::move(prom)) {}

  // Called only once unlock() has already handed this waiter the lock
  // (mutex::unlock() hands off rather than clearing state_ - see its own
  // doc comment), so constructing the lock_guard here doesn't need to
  // touch state_ itself at all.
  void run() final { promise_.set_value(lock_guard(mutex_)); }

  void abandon() noexcept final {
    promise_.set_exception(
        std::make_exception_ptr(std::runtime_error("mutex destroyed while acquire() was pending")));
  }

  // operator new/delete inherited from detail::current_allocator_new_delete<T>
  // (est:util.current_loop) - see lock_resume_node's own doc comment
  // (just above) for why.

private:
  mutex& mutex_;
  promise<lock_guard> promise_;
};

// Same reasoning as lock() above: only current_allocator() is needed,
// never current_loop() itself.
inline auto mutex::acquire() -> future<lock_guard> {
  if (state_ == 0) {
    state_ = 1;
    return make_ready_future<lock_guard>(*this);
  }
  auto [prom, fut] = detail::make_promise_future_impl<lock_guard>(current_allocator());
  auto* node = new acquire_resume_node(*this, std::move(prom));
  waiters_.enqueue(*node);
  return std::move(fut);
}

} // namespace est
