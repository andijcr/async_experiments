export module est:sync.mutex;

import :util.intrusive_list;

export namespace est {

// A waiting party's link into est::mutex's own waiter list - a distinctly
// named alias for est::intrusive_list_node (est:util.intrusive_list), the
// generic intrusive-list node this project's other waiter/ready-queue-
// shaped structures (est::future_state<T>'s pending continuations,
// est::loop's ready-queue entries) are also built on directly. Kept as
// its own name here, not spelled out as est::intrusive_list_node at every
// use site, so est::mutex's own public API (enqueue()/dequeue() below)
// reads as "a waiting party," not as a generic list detail leaking
// through.
using mutex_waiter = intrusive_list_node;

// Deliberately minimal: one `int` lock word plus an intrusive_list of
// waiting parties. No OS object, no syscall, no allocation.
//
// lock()/unlock() are bookkeeping only right now - there's nothing to
// actually protect yet in a single-threaded design with no real
// interrupts modeled (docs/PLAN.md). The eventual job here is guarding
// a waiter list against reentrancy - e.g. a timer or I/O completion
// arriving from an interrupt context while mainline code is enqueueing
// or draining that same list - but that protection is deliberately not
// built speculatively into a hosted-Linux backend that has nothing to
// guard against; it arrives (as a platform hook again, or something
// else) when a backend that actually needs it exists (bare metal, or a
// future multi-loop). It is not a general-purpose thread mutex and
// isn't trying to be one.
class mutex {
public:
  mutex() noexcept = default;
  mutex(const mutex&) = delete;
  auto operator=(const mutex&) -> mutex& = delete;
  mutex(mutex&&) = delete;
  auto operator=(mutex&&) -> mutex& = delete;
  ~mutex() = default;

  void lock() noexcept { state_ = 1; }

  void unlock() noexcept { state_ = 0; }

  [[nodiscard]] auto locked() const noexcept -> bool { return state_ != 0; }

  // Caller must hold the lock.
  void enqueue(mutex_waiter& waiter) noexcept { waiters_.enqueue(waiter); }

  // Caller must hold the lock. Returns nullptr if the list is empty.
  [[nodiscard]] auto dequeue() noexcept -> mutex_waiter* { return waiters_.dequeue(); }

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return !waiters_.empty(); }

private:
  int state_ = 0;
  intrusive_list<mutex_waiter> waiters_;
};

} // namespace est
