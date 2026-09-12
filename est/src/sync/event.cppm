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
// counting_event that enqueued them, so hoisting it out here avoids an
// identical resume_node type per Mode instantiation. Matches
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
// Holds no `loop&` of its own (unlike future_state<T> - see that class's
// own doc comment): wait()/set()/~counting_event() each resolve
// est::current_loop()/est::current_allocator() fresh, at the point of
// use - only current_allocator(), not current_loop() itself, where a
// method never actually needs the loop (wait()/~counting_event(); set()
// still needs current_loop() for enqueue_ready()). There is no
// constructor taking an explicit `loop&` either. KNOWN HAZARD this
// creates, same as est::mutex's own doc comment describes: a waiter
// queued by wait() carries a future_state<void> built against whichever
// loop was current *then*; set()/~counting_event() resolve
// current_loop() fresh, *now* - if the current loop has changed in
// between, a waiter can be handed to (or destroyed through the
// allocator of) a different loop than the one it actually belongs to.
// A cached `loop&` member ruled this out structurally; resolving fresh
// does not.
//
// max_count bounds how high the count is ever allowed to climb: set(n)
// (below) saturates at it rather than growing without limit, the same
// way std::counting_semaphore<LeastMaxValue> bounds release() - except
// saturating, a deliberate choice for this primitive specifically, rather
// than std::counting_semaphore's own undefined-behavior-on-overflow
// contract: est::binary_event (below) needs set() to be a safe no-op once
// already signaled, callable from more than one place without every
// caller first checking whether someone else already signaled it - a
// precondition violation here would defeat that outright, whereas
// est::mutex/est::shared_ptr's own preconditions elsewhere in this
// codebase are all check()-enforced, not saturated, since violating
// those really does indicate a caller bug worth catching. Defaults to an
// effectively-unbounded value (a plain semaphore/counting event with no
// meaningful ceiling); est::binary_event (below) is nothing more than a
// counting_event<Mode> constructed with
// max_count = 1 - not a separate implementation.
template <EventResetMode Mode> class counting_event {
public:
  explicit counting_event(int max_count = std::numeric_limits<int>::max()) noexcept
      : max_count_(max_count) {
    check(max_count > 0, "counting_event: max_count must be positive");
  }

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
  // waiters_.empty() checked *before* resolving current_allocator() - a
  // counting_event with nothing queued can legitimately be destroyed
  // long after whatever loop was current when it was created has stopped
  // being current at all, and current_allocator() would fail its own
  // precondition in that case even though nothing here actually needs
  // one. Only the allocator, not the loop itself, is needed to destroy
  // an abandoned waiter node - current_loop() is never resolved here at
  // all.
  ~counting_event() {
    if (waiters_.empty()) {
      return;
    }
    auto allocator = current_allocator();
    waiters_.drain([allocator](detail::ready_node& node) { node.destroy(allocator, false); });
  }

  [[nodiscard]] auto count() const noexcept -> int { return count_; }

  [[nodiscard]] auto max_count() const noexcept -> int { return max_count_; }

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return !waiters_.empty(); }

  // Increments the count by up to n (default 1), saturating at
  // max_count - i.e. actually adding min(n, max_count - count()) - and
  // wakes waiters:
  //  - automatic: hands the (possibly saturated) increment directly to
  //    that many currently queued waiters, one unit each - exactly like
  //    mutex::unlock() hands the lock directly to the next waiter,
  //    generalized to more than one of them. Any surplus (more added
  //    than the number of currently queued waiters) stays in the count
  //    for a later wait() to consume immediately via its own fast path,
  //    one unit per call.
  //  - manual: wakes *every* currently queued waiter unconditionally -
  //    there is nothing to "consume" for a manual-reset signal - and
  //    leaves the count exactly as incremented, so every *future*
  //    wait() also takes the fast path, until reset().
  // A set() that can't add anything (count() already at max_count) is a
  // no-op - not a checked precondition violation - the same reasoning
  // one_shot_event::set() below applies to a redundant call: a caller
  // shouldn't have to guard set() with its own "is there room?" check
  // just to safely call it from more than one place. This is also
  // exactly what makes est::binary_event's set() idempotent once
  // signaled, with no code of its own: max_count = 1 already saturates
  // after the first successful call.
  //
  // Returns the amount actually added (min(n, max_count - count()), the
  // same value stored into count()/handed to waiters above) - 0 for a
  // no-op call, up to n otherwise - so a caller that cares whether it was
  // silently capped (unlike est::binary_event/est::one_shot_event, which
  // exist precisely so their own callers don't have to care) can check
  // the result instead of it being entirely unobservable.
  //
  // Deferred through est::loop::enqueue_ready() rather than completed
  // directly here, for the identical reason mutex::unlock() defers
  // (see its own doc comment): detail::event_resume_node::run() only ever
  // calls promise_.set_value(), never runs arbitrary downstream coroutine
  // code inline on this call stack, so nothing about set() itself needs
  // to bound recursion - deferring anyway keeps this consistent with
  // every other completion path in this codebase: never invoke a
  // continuation inline.
  // current_loop() resolved only once it's known there's at least one
  // queued waiter to hand off to, not unconditionally on every successful
  // set() - a caller that sets an event with nothing queued (the common
  // case for a manual-reset event set ahead of its first wait()) doesn't
  // need a loop current at all, and current_loop() would fail its own
  // precondition in that case otherwise.
  auto set(int n = 1) -> int {
    check(n > 0, "counting_event::set(n) requires n > 0");
    const int actual_n = std::min(n, max_count_ - count_);
    if (actual_n <= 0) {
      return 0;
    }
    count_ += actual_n;
    if (!waiters_.empty()) {
      auto& loop_ref = current_loop();
      if constexpr (Mode == EventResetMode::automatic) {
        for (int i = 0; i < actual_n; ++i) {
          auto* waiter = waiters_.dequeue();
          if (waiter == nullptr) {
            break;
          }
          --count_;
          loop_ref.enqueue_ready(*waiter);
        }
      } else {
        waiters_.drain([&loop_ref](detail::ready_node& node) { loop_ref.enqueue_ready(node); });
      }
    }
    return actual_n;
  }

  // Clears the count without touching anything already handed off - any
  // waiter set() already enqueue_ready()'d still resolves normally
  // (loop::enqueue_ready() has no way to "unschedule" a node, nor would
  // that be safe once a coroutine might already be about to resume from
  // it); only a count that hasn't yet been consumed by a waiter is
  // cleared. Equally meaningful for either Mode: automatic's own
  // wait() already self-clears one unit per successful wait, but
  // nothing stops a caller from wanting to drop a surplus (more added
  // by set() than there were waiters to hand it to) without waiting it
  // out.
  void reset() noexcept { count_ = 0; }

  // Suspends the calling coroutine until the count is greater than zero,
  // resuming immediately (no suspension, no allocation - see
  // future_awaiter<T>::await_ready(), est:future) if it already is. Only
  // current_allocator() is needed here, never current_loop() itself - the
  // fast path completes the promise inline, and the slow path only
  // enqueues into this event's own waiters_, not onto any loop's
  // ready-queue (that happens later, from set()).
  [[nodiscard]] auto wait() -> future<void> {
    auto allocator = current_allocator();
    auto [prom, fut] = detail::make_promise_future_impl<void>(allocator);
    if (count_ > 0) {
      if constexpr (Mode == EventResetMode::automatic) {
        --count_;
      }
      prom.set_value();
      return std::move(fut);
    }
    auto* node = allocator.template new_object<detail::event_resume_node>(std::move(prom));
    waiters_.enqueue(*node);
    return std::move(fut);
  }

private:
  int count_ = 0;
  int max_count_;
  intrusive_list<detail::ready_node> waiters_;
};

// A counting_event<Mode> whose count never exceeds one - the classic
// Win32 event object (CreateEvent/SetEvent/ResetEvent), generalized only
// by which reset mode Mode selects. Not a separate implementation with
// its own set()/wait()/reset(): literally counting_event<Mode> constructed
// with max_count = 1, nothing else - the base class's own set(n)
// saturating at max_count (see its own doc comment) already gives
// set()'s idempotent-once-signaled behavior for free, matching SetEvent()'s
// own documented "calling it again before anything has consumed the
// signal is a no-op" behavior with no code of this class's own. Only
// adds signaled() (a named alias for count() > 0) and a constructor that
// hardcodes max_count = 1 - inheriting counting_event<Mode>'s own
// constructor via a using-declaration isn't an option here, since that
// would also inherit its own (unbounded) max_count default.
template <EventResetMode Mode> class binary_event : public counting_event<Mode> {
public:
  binary_event() noexcept : counting_event<Mode>(1) {}

  [[nodiscard]] auto signaled() const noexcept -> bool { return this->count() > 0; }
};

// A binary_event<Mode> that may be set() at most once, ever - a single
// signal, handed out to every interested waiter (Mode = manual, the
// common case - one
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
// set() past the first call is a no-op, not a checked precondition
// violation - matching counting_event<Mode>::set()'s own saturating
// idempotency once max_count is reached, one layer down: forcing every
// caller to guard set() with its own "have I already signaled this?"
// check, on pain of aborting the whole process, would defeat the point
// of a primitive meant to let independent callers - e.g. two unrelated
// cancellation sources racing to fire the same one-shot signal - all
// safely call set() without coordinating first.
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

  // Returns 1 for the call that actually signals (mirroring
  // counting_event<Mode>::set()'s own "amount actually added" return
  // value - always 1 here, since binary_event<Mode>::set() can only ever
  // add its single unit on this, the first and only call reaching it) or
  // 0 for a redundant call that has_been_set_ turned into a no-op before
  // ever reaching binary_event<Mode>::set() at all.
  auto set() -> int {
    if (has_been_set_) {
      return 0;
    }
    has_been_set_ = true;
    return binary_event<Mode>::set();
  }

  void reset() = delete;

private:
  bool has_been_set_ = false;
};

} // namespace est
