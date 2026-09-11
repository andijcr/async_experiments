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
// Holds a `loop&` (same convention as future_state<T> - see that class's
// own doc comment) rather than its own allocator: `lock()`/`acquire()`
// each build a `future_state<T>` against it
// (`make_promise_future<T>(loop_)`), and `unlock()`/`~mutex()` need its
// allocator to destroy a waiter node. Same lifetime precondition as every
// other loop-holding type in this codebase: the loop must outlive every
// mutex constructed against it.
class mutex {
public:
  explicit mutex(loop& loop_ref) noexcept : loop_(loop_ref) {}

  // Sugar over the constructor above using est::current_loop()
  // (est:util.current_loop) instead of a caller-supplied loop&.
  mutex() noexcept : mutex(current_loop()) {}

  mutex(const mutex&) = delete;
  auto operator=(const mutex&) -> mutex& = delete;
  mutex(mutex&&) = delete;
  auto operator=(mutex&&) -> mutex& = delete;

  // Destroys (without granting the lock) any waiter still queued on
  // lock() or acquire() - mirrors future_state<T>::~future_state() and
  // loop::~loop() (both drain their own pending lists the same way, for
  // the same reason). Each waiter node's own destroy() (below) completes
  // its promise with an exception first, rather than just deallocating
  // itself silently - see lock_resume_node's own doc comment for why
  // that matters beyond just freeing the node itself.
  ~mutex() {
    waiters_.drain([this](detail::ready_node& node) { node.destroy(loop_.allocator(), false); });
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
  // Abandonment is only safe because destroy() (never run()) completes
  // with an exception instead of a real value - see lock_resume_node's
  // own doc comment. Deferring through the loop keeps that guard rail
  // intact: whether a queued waiter is later run() (the loop drains it
  // normally) or destroy()'d (the loop or this mutex is torn down first)
  // is decided at drain time, not decided in advance here.
  //
  // KNOWN LIMITATION left open by keeping this deferred:
  // acquire_resume_node::run() still needs a live mutex& to construct the
  // lock_guard it completes with. If this mutex is destroyed after
  // unlock() enqueues a waiter but before the loop actually drains it -
  // only possible if the loop outlives this mutex and keeps running
  // during that gap, since ~mutex() itself drains waiters_ via destroy(),
  // never run() - that waiter's run() would construct a lock_guard over
  // an already-dangling mutex&. Left as a documented precondition: the
  // loop must outlive every mutex constructed against it, and a caller
  // must not let the loop keep running past a mutex's destruction while
  // a waiter is still queued on it.
  void unlock() noexcept {
    if (auto* waiter = waiters_.dequeue()) {
      loop_.enqueue_ready(*waiter);
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
  loop& loop_;
  intrusive_list<detail::ready_node> waiters_;
};

// The waiter node behind lock()'s slow path (mutex already locked):
// queued in mutex::waiters_, completing a est::promise<void> once
// unlock() hands it the lock.
//
// destroy() completing the promise with an exception (rather than simply
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
// (loop::~loop()). `destroy()`'s own `ran` parameter (est::detail::
// ready_node's own doc comment, est:loop) stops this from
// double-completing an already-successfully-completed promise:
// `destroy()` here can run either after the loop has already drained
// this node via `run()` (unlock() handed it the lock, `ran` true), or,
// for a node still sitting in `waiters_` or still queued on the loop's
// own ready_ list, from `~mutex()`'s or `~loop()`'s own drain without
// `run()` ever having been called (`ran` false).
class mutex::lock_resume_node final : public detail::ready_node {
public:
  explicit lock_resume_node(promise<void> prom) noexcept : promise_(std::move(prom)) {}

  void run() final { promise_.set_value(); }

  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept final {
    if (!ran) {
      promise_.set_exception(
          std::make_exception_ptr(std::runtime_error("mutex destroyed while lock() was pending")));
    }
    allocator.delete_object(this);
  }

private:
  promise<void> promise_;
};

inline auto mutex::lock() -> future<void> {
  auto [prom, fut] = make_promise_future<void>(loop_);
  if (state_ == 0) {
    state_ = 1;
    prom.set_value();
    return std::move(fut);
  }
  auto* node = loop_.allocator().template new_object<lock_resume_node>(std::move(prom));
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
// comment for why destroy() needs the same `ran`-guarded
// exception-completion on the abandoned (never handed the lock) path.
// mutex_ staying valid for run()'s use relies on the precondition
// unlock()'s own doc comment documents as a known, open limitation.
class mutex::acquire_resume_node final : public detail::ready_node {
public:
  acquire_resume_node(mutex& mutex_ref, promise<lock_guard> prom) noexcept
      : mutex_(mutex_ref), promise_(std::move(prom)) {}

  // Called only once unlock() has already handed this waiter the lock
  // (mutex::unlock() hands off rather than clearing state_ - see its own
  // doc comment), so constructing the lock_guard here doesn't need to
  // touch state_ itself at all.
  void run() final { promise_.set_value(lock_guard(mutex_)); }

  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept final {
    if (!ran) {
      promise_.set_exception(std::make_exception_ptr(
          std::runtime_error("mutex destroyed while acquire() was pending")));
    }
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
