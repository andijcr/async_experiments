export module est:sync.mutex;

export namespace est {

// An intrusive singly-linked list node for parties waiting on a mutex.
// Deliberately payload-free at this layer: est::future's shared_state
// (M2) will embed one of these to link itself into a mutex's waiter
// list, without the mutex needing to know what a future is.
class mutex_waiter {
public:
  mutex_waiter* next = nullptr;
};

// The intrusive singly-linked LIFO list mutex itself is built on,
// extracted into its own type so a caller that only needs waiter-list
// bookkeeping - not the lock word wrapped around it - can use it
// directly (est::future's shared_state, M2: nothing there is
// concurrent, so there was never anything for mutex's lock()/unlock()
// to actually protect - see docs/PLAN.md). LIFO is the natural order
// for a singly-linked list; nothing in this project needs FIFO
// fairness among waiters.
class waiter_list {
public:
  void enqueue(mutex_waiter& waiter) noexcept {
    waiter.next = head_;
    head_ = &waiter;
  }

  [[nodiscard]] auto dequeue() noexcept -> mutex_waiter* {
    auto* head = head_;
    if (head != nullptr) {
      head_ = head->next;
      head->next = nullptr;
    }
    return head;
  }

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return head_ != nullptr; }

private:
  mutex_waiter* head_ = nullptr;
};

// Deliberately minimal: one `int` lock word plus a waiter_list of
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

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return waiters_.has_waiters(); }

private:
  int state_ = 0;
  waiter_list waiters_;
};

} // namespace est
