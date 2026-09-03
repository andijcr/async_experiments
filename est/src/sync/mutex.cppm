module;

export module est:sync.mutex;

import :platform;

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
// allocation. lock()/unlock() go through Platform's critical-section
// primitive - the actual protection (interrupt mask on bare metal; a
// no-op on the hosted, single-threaded backend), not `state_` itself,
// which only exists for the locked() debug/test observer below. Its
// job, established here and consumed by est::future's shared_state in
// M2, is guarding a waiter list against reentrancy: e.g. a timer or I/O
// completion arriving from an interrupt context while mainline code is
// enqueueing or draining that same list. It is not a general-purpose
// thread mutex and isn't trying to be one.
template <class Platform = platform::hosted_linux> class basic_mutex {
public:
  basic_mutex() noexcept = default;
  basic_mutex(const basic_mutex&) = delete;
  auto operator=(const basic_mutex&) -> basic_mutex& = delete;
  basic_mutex(basic_mutex&&) = delete;
  auto operator=(basic_mutex&&) -> basic_mutex& = delete;
  ~basic_mutex() = default;

  void lock() noexcept {
    Platform::enter_critical_section();
    state_ = 1;
  }

  void unlock() noexcept {
    state_ = 0;
    Platform::leave_critical_section();
  }

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

using mutex = basic_mutex<>;

} // namespace est
