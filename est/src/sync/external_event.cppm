export module est:sync.external_event;

import std;
import :check;
import :future;
import :loop;
import :sync.event;

export namespace est {

// A tiny, copyable handle any external context (an ISR, another thread -
// the same "another thread, an ISR, a hardware register" set
// external_event<T>'s own doc comment already names) can call after
// writing a new value through its own atomic, notifying the loop that
// owns a specific external_event<T> registration to poll it soon
// instead of only on the next caller-driven schedule_periodic() tick.
// Obtained via external_event<T>::notifier() below - nothing else in est
// constructs one.
//
// Deliberately holds a plain loop* rather than anything thread-local: an
// external context calling notify() might not be the loop's own thread
// at all (a producer std::jthread, an ISR that happens to share the
// loop thread's own TLS block or not, depending on the backend - see
// docs/PLAN.md's issue #125 entry for the full reasoning) - a loop*
// captured once, on the loop's own thread, at notifier()-construction
// time, sidesteps the question entirely rather than depending on
// whichever platform::instance() the *calling* context happens to
// resolve. notify() itself only ever touches loop::notify_external()
// (est:loop) - which itself does the atomic store *and* the
// platform::instance().wake() call, resolved fresh, deliberately,
// wherever notify_external() actually runs (see wake()'s own doc
// comment in platform.cppm for why that one *is* meant to be
// thread-local-dispatched) - both already documented safe to call from
// any context that can call anything at all.
//
// One real precondition follows from that last point though:
// platform::instance() is thread_local (platform.cppm), and nothing
// installs a backend on a freshly spawned OS thread automatically - a
// genuinely separate producer thread calling notify() needs its own
// platform::interface installed first (typically the identical backend
// object the loop thread itself uses, via
// platform::override_instance()), the same as any other thread that
// wants to drive an est::loop already needs (est/tests/test_main.cpp's
// own installation, on the main thread, being the one example every
// test in this codebase already relies on without a second thought).
// This is a non-issue on a single-core bare-metal target calling
// notify() from an ISR instead - there, mainline and the ISR share the
// identical TLS block by construction (confirmed empirically; see
// docs/PLAN.md's issue #125 entry), so nothing extra is needed.
class external_notifier {
public:
  void notify() const noexcept { owner_->notify_external(); }

private:
  template <class T>
    requires std::equality_comparable<T>
  friend class external_event;

  explicit external_notifier(loop& owner) noexcept : owner_(&owner) {}

  loop* owner_;
};

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
// Two ways to drive poll(): a caller wires it into a periodic timer
// explicitly -
//
//   std::atomic<int> reading{0};             // written by another thread/ISR
//   est::external_event<int> bridge{reading};
//   auto handle = est::schedule_periodic(50ms, [&bridge] { bridge.poll(); });
//   // ... co_await bridge.wait(); use bridge.value(); bridge.reset();
//
// - or, via the second constructor below, registers with a loop
// directly and lets it call poll() at its own dispatch checkpoints
// instead, closing the poll-period latency the timer-driven style above
// always pays (issue #125):
//
//   std::atomic<int> reading{0};
//   est::external_event<int> bridge{reading, loop};
//   auto notifier = bridge.notifier();       // handed to the external writer
//   // external context: reading.store(v, release); notifier.notify();
//   // loop thread, as before: co_await bridge.wait(); bridge.value(); bridge.reset();
//
// Neither is owned by this class in the sense of a schedule_periodic()
// of its own (est:timer.periodic). The timer-driven style also lets one
// periodic timer's callback drive several sources' poll() calls at once
// if a caller wants that, instead of forcing one timer per source, and
// keeps this class trivially testable without any real second thread: a
// test assigns directly to the std::atomic<T> it constructs the bridge
// over, single-threaded, matching every other test in this codebase.
template <class T>
  requires std::equality_comparable<T>
class external_event : private detail::external_source {
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

  // Opt-in loop registration: `owner` calls poll() on its own, the next
  // time it notices notify_external() was called since the last check
  // (est::loop::poll_external_if_pending(), est:loop) - instead of this
  // needing a caller-supplied schedule_periodic() timer at all. Fully
  // additive: the constructor above still works exactly as before for a
  // caller that doesn't want this coupling. Unregisters in ~external_event()
  // below - the same "loop must outlive me" precondition every other
  // loop-adjacent type in this codebase already carries (future_state<T>,
  // est::mutex, est::counting_event<Mode>, ...), not a new hazard class.
  external_event(std::atomic<T>& source, loop& owner)
      : source_(&source), last_seen_(source.load(std::memory_order_relaxed)), owner_(&owner) {
    owner_->register_external(*this);
  }

  external_event(const external_event&) = delete;
  auto operator=(const external_event&) -> external_event& = delete;
  external_event(external_event&&) = delete;
  auto operator=(external_event&&) -> external_event& = delete;

  ~external_event() override {
    if (owner_ != nullptr) {
      owner_->unregister_external(*this);
    }
  }

  // Loop-thread only; never suspends. The only read of source_ after
  // construction (see the constructor's own relaxed load for the initial
  // value). memory_order_acquire pairs with the external writer's own
  // release store (or stronger) - a caller writing `source` from another
  // thread should use at least memory_order_release there for this to
  // actually synchronize-with the read here, not just happen to observe
  // the new value on a given platform. `override`: also
  // detail::external_source's own pure virtual (est:loop) - the loop
  // registration constructor above hands `*this` to
  // loop::register_external() as exactly that base, so this method is
  // what a registered loop actually calls; unrelated to (and no
  // different for) a caller that drives this by hand instead.
  void poll() noexcept override {
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

  // Only valid on an instance built with the loop-registering constructor
  // above - est::check()'s failure is this method's answer for one that
  // wasn't (the caller-driven-poll() constructor has no loop to hand a
  // notifier a reference to in the first place). See external_notifier's
  // own doc comment for what the returned handle actually does and why
  // it's safe to call from any context.
  [[nodiscard]] auto notifier() const noexcept -> external_notifier {
    check(owner_ != nullptr,
          "est::external_event<T>::notifier() requires the loop-registering constructor");
    return external_notifier(*owner_);
  }

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
  loop* owner_ = nullptr;
};

} // namespace est
