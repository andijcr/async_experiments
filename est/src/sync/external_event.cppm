export module est:sync.external_event;

import std;
import :future;
import :sync.event;

export namespace est {

// Bridges a value written from outside this loop's own call stack -
// another thread, an ISR, a hardware register on a future bare-metal
// target - into est's cooperative event system
// (est::binary_event<EventResetMode::manual>, est:sync.event). `source`
// is the *only* atomic anywhere in this framework an external writer
// ever touches: it stores through it at its own pace, and the
// constructor (once, for the initial value) plus poll() below
// (loop-thread only, every time after) are the only places this class
// ever reads it. Between poll() calls, nothing here is shared/concurrently
// touched: the
// cached last-seen value and the internal binary_event live entirely on
// the loop thread, the same single-threaded assumption as everywhere
// else in est - this class exists to be the one deliberate seam where
// that assumption meets a genuinely external writer.
//
// Deliberately not started/owned by this class: a caller wires poll()
// into a periodic timer explicitly -
//
//   std::atomic<int> reading{0};             // written by another thread/ISR
//   est::external_event<int> bridge{reading};
//   auto handle = est::schedule_periodic(50ms, [&bridge] { bridge.poll(); });
//   // ... co_await bridge.wait(); use bridge.value(); bridge.reset();
//
// - rather than external_event owning a schedule_periodic() of its own
// (est:timer.periodic). That lets one periodic timer's callback drive
// several sources' poll() calls at once if a caller wants that, instead
// of forcing one timer per source, and keeps this class trivially
// testable without any real second thread: a test assigns directly to
// the std::atomic<T> it constructs the bridge over, single-threaded,
// matching every other test in this codebase.
template <class T>
  requires std::equality_comparable<T>
class external_event {
public:
  // T must be lock-free: poll() is meant to be callable from a context as
  // constrained as a periodic timer callback (eventually: an ISR), so it
  // must never block on a fallback lock the way a non-lock-free
  // std::atomic<T> silently could.
  static_assert(std::atomic<T>::is_always_lock_free,
                "est::external_event<T>: T must be lock-free "
                "(std::atomic<T>::is_always_lock_free)");

  explicit external_event(std::atomic<T>& source) noexcept
      : source_(&source), last_seen_(source.load(std::memory_order_relaxed)) {}

  // Loop-thread only; never suspends. The only read of source_ after
  // construction (see the constructor's own relaxed load for the initial
  // value). memory_order_acquire pairs with the external writer's own
  // release store (or stronger) - a caller writing `source` from another
  // thread should use at least memory_order_release there for this to
  // actually synchronize-with the read here, not just happen to observe
  // the new value on a given platform.
  void poll() noexcept {
    T current = source_->load(std::memory_order_acquire);
    if (current != last_seen_) {
      last_seen_ = current;
      event_.set(); // binary_event<manual>::set() - idempotent, stays
                    // signaled until reset()
    }
  }

  [[nodiscard]] auto wait() -> future<void> { return event_.wait(); }

  // Clears the signal so the next poll() that finds source_ changed
  // again wakes wait() afresh. Without an explicit reset step, a second
  // change would be silently swallowed: binary_event::set() past the
  // first successful call is a no-op until reset() (see its own doc
  // comment) - the consumer, not poll(), decides when it's done
  // observing the current value and ready for the next one.
  void reset() noexcept { event_.reset(); }

  [[nodiscard]] auto value() const noexcept -> T { return last_seen_; }

private:
  // A pointer, not a reference: est::binary_event<Mode> (event_, below)
  // already deletes every special member counting_event<Mode> itself
  // deletes, so this class is non-copyable/non-movable regardless of
  // source_'s own type - a pointer costs nothing extra here, and avoids
  // cppcoreguidelines-avoid-const-or-ref-data-members's own concern
  // (a reference member forbids even generating an implicit assignment
  // operator, which this class has no other reason to be missing).
  std::atomic<T>* source_;
  T last_seen_;
  binary_event<EventResetMode::manual> event_;
};

} // namespace est
