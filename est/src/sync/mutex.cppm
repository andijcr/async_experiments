module;

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

// Deliberately minimal: one `int` lock word plus a pointer to an
// intrusive list of waiting parties. No OS object, no syscall, no
// allocation.
//
// lock()/unlock() are bookkeeping only right now - there's nothing to
// actually protect yet in a single-threaded design with no real
// interrupts modeled (docs/PLAN.md). The eventual job here, consumed by
// est::future's shared_state in M2, is guarding a waiter list against
// reentrancy - e.g. a timer or I/O completion arriving from an
// interrupt context while mainline code is enqueueing or draining that
// same list - but that protection is deliberately not built
// speculatively into a hosted-Linux backend that has nothing to guard
// against; it arrives (as a platform hook again, or something else)
// when a backend that actually needs it exists (bare metal, or a future
// multi-loop). It is not a general-purpose thread mutex and isn't
// trying to be one.
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

  // Caller must hold the lock. Pushes onto the waiter list (LIFO - the
  // natural order for a singly-linked list; M1 has no need for FIFO
  // fairness among waiters).
  void enqueue(mutex_waiter& waiter) noexcept {
    waiter.next = waiters_;
    waiters_ = &waiter;
  }

  // Caller must hold the lock. Returns nullptr if the list is empty.
  [[nodiscard]] auto dequeue() noexcept -> mutex_waiter* {
    auto* head = waiters_;
    if (head != nullptr) {
      waiters_ = head->next;
      head->next = nullptr;
    }
    return head;
  }

  [[nodiscard]] auto has_waiters() const noexcept -> bool { return waiters_ != nullptr; }

private:
  int state_ = 0;
  mutex_waiter* waiters_ = nullptr;
};

} // namespace est
