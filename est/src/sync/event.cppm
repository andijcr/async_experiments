export module est:sync.event;

import std;
import :check;
import :future;
import :loop;
import :promise;
import :util.current_loop;
import :util.intrusive_list;

export namespace est {

// How a signal is consumed: `automatic` decrements the count by exactly
// one on every successful wait() (a coroutine-aware counting semaphore -
// std::counting_semaphore's acquire()/release(), generalized to
// suspend rather than block); `manual` leaves the count untouched on
// wait() - once signaled, *every* waiter, present and future, succeeds
// immediately, until an explicit reset() clears it. The classic Win32
// distinction (CreateEvent's bManualReset), generalized here to a
// multi-unit count rather than a plain boolean.
enum class EventResetMode : std::uint8_t { automatic, manual };

} // namespace est

namespace est::detail {

// The waiter node behind counting_event<Mode>::wait()'s slow path (not yet
// signaled): queued in counting_event<Mode>::waiters_, completing an
// est::promise<void> once set() hands it a unit. No coroutine_handle in
// sight here, mirroring mutex::lock_resume_node exactly - wait() itself is
// no coroutine-only primitive, so this node only ever has to know how to
// complete a promise.
//
// Deliberately *not* nested inside counting_event<Mode> (unlike
// mutex::lock_resume_node inside the non-template mutex): run()/destroy()
// only ever touch promise_, never Mode or anything else about the
// counting_event that enqueued them - review caught the first version of
// this file defining an identical resume_node type once per Mode
// instantiation for no reason. Hoisted out here instead, matching
// est::detail::ready_node/timer_node's own "type-erased, internal-only"
// placement (est:loop) - counting_event<Mode>::wait() (below) is the only
// caller either way, on both Mode values.
//
// destroy() completing the promise with an exception (rather than simply
// deallocating the node) matters for the identical reason
// mutex::lock_resume_node's own doc comment gives in full: a coroutine
// suspended on the future<void> wait() returned holds that future_state
// alive via its own frame, reachable only once this future_state itself
// completes - silently dropping the promise instead would strand that
// frame forever, with neither side able to free the other first.
// Completing it here (whether from set()'s own successful hand-off, `ran`
// true, or from ~counting_event()'s/loop::~loop()'s abandonment drain,
// `ran` false) always drains the future_state's own pending continuation
// onto the loop's ready queue, so the frame is never stranded either way.
class event_resume_node final : public ready_node {
public:
  explicit event_resume_node(promise<void> prom) noexcept : promise_(std::move(prom)) {}

  void run() final { promise_.set_value(); }

  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept final {
    if (!ran) {
      promise_.set_exception(std::make_exception_ptr(
          std::runtime_error("counting_event destroyed while wait() was pending")));
    }
    allocator.delete_object(this);
  }

private:
  promise<void> promise_;
};

} // namespace est::detail

export namespace est {

// A cooperative-scheduling counting event: an awaitable generalization of
// a counting semaphore, built the same way est::mutex is (est:sync.mutex,
// see its own top comment for the underlying rationale) - a plain int
// count, an est::intrusive_list<detail::ready_node> waiters_ queue, and
// detail::event_resume_node (above) completing an est::promise<void> once
// a waiter is satisfied. wait() returns a plain future<void>, exactly
// like mutex::lock() - already-satisfied wait() resumes immediately,
// through future_awaiter<T>'s own already-ready fast path (est:future),
// with no extra allocation or suspension.
//
// est::binary_event<Mode> and est::one_shot_event<Mode> (both below) are
// derived from this - not separate implementations - each one layering
// exactly one more constraint on top of set()/wait()/reset(), reusing
// this class's own waiters_ mechanism unchanged all the way down.
//
// Holds a `loop&` (M3's convention, same as future_state<T>/mutex - see
// their own doc comments) rather than its own allocator: wait() builds a
// future_state<void> against it, and set()/~counting_event() need its
// allocator to enqueue/destroy waiter nodes. Same lifetime precondition
// as every other loop-holding type in this codebase: the loop must
// outlive every counting_event constructed against it.
template <EventResetMode Mode> class counting_event {
public:
  explicit counting_event(loop& loop_ref) noexcept : loop_(loop_ref) {}

  // Issue #30: sugar over the constructor above using est::current_loop()
  // (est:util.current_loop) instead of a caller-supplied loop&.
  counting_event() noexcept : counting_event(current_loop()) {}

  counting_event(const counting_event&) = delete;
  auto operator=(const counting_event&) -> counting_event& = delete;
  counting_event(counting_event&&) = delete;
  auto operator=(counting_event&&) -> counting_event& = delete;

  // Destroys (without satisfying) any waiter still queued on wait() -
  // mirrors mutex::~mutex() and future_state<T>::~future_state() (both
  // drain their own pending lists the same way, for the same reason).
  // Each waiter node's own destroy() (detail::event_resume_node, above)
  // completes its promise with an exception first, rather than just
  // deallocating itself silently - see its own doc comment for why that
  // matters beyond just freeing the node itself.
  ~counting_event() {
    waiters_.drain([this](detail::ready_node& node) { node.destroy(loop_.allocator(), false); });
  }

  [[nodiscard]] auto count() const noexcept -> int { return count_; }

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return !waiters_.empty(); }

  // Increments the count by n (default 1) and wakes waiters:
  //  - automatic: hands the increment directly to up to n currently
  //    queued waiters, one unit each - exactly like mutex::unlock()
  //    hands the lock directly to the next waiter, generalized to up to
  //    n of them. Any surplus (n greater than the number of currently
  //    queued waiters) stays in the count for a later wait() to consume
  //    immediately via its own fast path, one unit per call.
  //  - manual: wakes *every* currently queued waiter unconditionally -
  //    there is nothing to "consume" for a manual-reset signal - and
  //    leaves the count exactly as incremented, so every *future*
  //    wait() also takes the fast path, until reset().
  // Deferred through est::loop::enqueue_ready() rather than completed
  // directly here, for the identical reason mutex::unlock() defers
  // (see its own doc comment): detail::event_resume_node::run() only ever
  // calls promise_.set_value(), never runs arbitrary downstream coroutine
  // code inline on this call stack, so nothing about set() itself needs
  // to bound recursion - deferring anyway keeps this consistent with
  // every other completion path in this codebase (M3, docs/PLAN.md:
  // never invoke a continuation inline).
  void set(int n = 1) {
    check(n > 0, "counting_event::set(n) requires n > 0");
    count_ += n;
    if constexpr (Mode == EventResetMode::automatic) {
      for (int i = 0; i < n; ++i) {
        auto* waiter = waiters_.dequeue();
        if (waiter == nullptr) {
          break;
        }
        --count_;
        loop_.enqueue_ready(*waiter);
      }
    } else {
      waiters_.drain([this](detail::ready_node& node) { loop_.enqueue_ready(node); });
    }
  }

  // Clears the count without touching anything already handed off - any
  // waiter set() already enqueue_ready()'d still resolves normally
  // (loop::enqueue_ready() has no way to "unschedule" a node, nor would
  // that be safe once a coroutine might already be about to resume from
  // it); only a count that hasn't yet been consumed by a waiter is
  // cleared. Equally meaningful for either Mode: automatic's own
  // wait() already self-clears one unit per successful wait, but
  // nothing stops a caller from wanting to drop a surplus (n larger
  // than the number of waiters at set() time) without waiting it out.
  void reset() noexcept { count_ = 0; }

  // Suspends the calling coroutine until the count is greater than zero,
  // resuming immediately (no suspension, no allocation - see
  // future_awaiter<T>::await_ready(), est:future) if it already is.
  [[nodiscard]] auto wait() -> future<void> {
    auto [prom, fut] = make_promise_future<void>(loop_);
    if (count_ > 0) {
      if constexpr (Mode == EventResetMode::automatic) {
        --count_;
      }
      prom.set_value();
      return std::move(fut);
    }
    auto* node = loop_.allocator().template new_object<detail::event_resume_node>(std::move(prom));
    waiters_.enqueue(*node);
    return std::move(fut);
  }

private:
  int count_ = 0;
  loop& loop_;
  intrusive_list<detail::ready_node> waiters_;
};

// A counting_event<Mode> whose count never exceeds one - the classic
// Win32 event object (CreateEvent/SetEvent/ResetEvent), generalized only
// by which reset mode Mode selects. set() (no argument, hiding the base
// class's set(int n) entirely - see below) is idempotent once already
// signaled: calling it again before anything has consumed the signal is
// a no-op, matching SetEvent()'s own documented behavior, rather than
// accumulating a count the way counting_event<Mode>::set(n) would.
// wait()/reset() are inherited unchanged - both already do exactly the
// right thing for a count clamped to {0, 1}, with nothing left for this
// class to add.
template <EventResetMode Mode> class binary_event : public counting_event<Mode> {
public:
  using counting_event<Mode>::counting_event;

  [[nodiscard]] auto signaled() const noexcept -> bool { return this->count() > 0; }

  // Declaring set() here hides *every* overload of the base class's own
  // set(int n) from ordinary (unqualified) lookup on a binary_event - not
  // an override (counting_event<Mode>::set() isn't virtual; nothing in
  // this hierarchy needs runtime dispatch, since every caller always
  // knows the concrete type it's holding), just plain C++ name hiding.
  // `binary_event_instance.set(3)` is therefore a compile error, not a
  // silently-accepted way to smuggle a count past the {0, 1} clamp this
  // class exists to enforce.
  void set() {
    if (!signaled()) {
      counting_event<Mode>::set(1);
    }
  }
};

// A binary_event<Mode> that may be set() at most once, ever - the
// motivating shape for est::event (issue #55): a single signal, handed
// out to every interested waiter (Mode = manual, the common case - one
// broadcast, seen by any number of independent wait() callers, present
// or future) or to exactly the first one to observe it (Mode =
// automatic - a single-consumer handoff, every later wait() blocks
// forever, nothing left to ever satisfy it).
//
// Tracks "has set() ever been called" with its own has_been_set_ flag
// rather than reusing signaled() for the check: for Mode = automatic,
// a successful wait() already decrements the count back to zero, so
// signaled() alone can't tell "never set()" apart from "set() once,
// already consumed" - exactly the distinction a one-shot guarantee
// needs to get right.
//
// reset() is deleted outright (again: hides the base's declaration from
// unqualified lookup, same technique set() above already uses, not an
// override) rather than merely documented-but-unchecked the way this
// codebase leaves some other preconditions (e.g. loop lifetime, where no
// runtime check is possible at all) - a caller resetting a one-shot event
// would silently defeat the one-shot guarantee this class exists to
// provide, and unlike a dangling reference, "don't call this method" is
// exactly the kind of precondition a compile error can enforce outright.
template <EventResetMode Mode> class one_shot_event : public binary_event<Mode> {
public:
  using binary_event<Mode>::binary_event;

  void set() {
    check(!has_been_set_, "one_shot_event::set() called more than once");
    has_been_set_ = true;
    binary_event<Mode>::set();
  }

  void reset() = delete;

private:
  bool has_been_set_ = false;
};

} // namespace est
