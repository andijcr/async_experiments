module;

#include <cassert>
#include <exception>
#include <memory_resource>
#include <utility>
#include <variant>

export module est:future;

import :sync.mutex;

export namespace est {

// The single owned object behind an est::promise<T>/est::future<T> pair
// - a view over shared state, not the state itself (docs/PLAN.md). Holds
// value-or-exception storage, a continuation slot built on the M1
// est::mutex's waiter list, and a plain (non-atomic - single-threaded,
// see est::mutex's own docs) reference count. Always heap-allocated via
// a std::pmr::polymorphic_allocator (see make_promise_future() in
// est:promise) - never constructed directly by a caller.
template <class T> class shared_state {
public:
  // Type-erased, allocator-constructed continuation, linked directly
  // into this shared_state's mutex waiter list - no second allocation
  // for the list node itself, exactly what est::mutex_waiter's
  // "payload-free at this layer" doc comment anticipated.
  class continuation_node : public mutex_waiter {
  public:
    continuation_node() = default;
    continuation_node(const continuation_node&) = delete;
    auto operator=(const continuation_node&) -> continuation_node& = delete;
    continuation_node(continuation_node&&) = delete;
    auto operator=(continuation_node&&) -> continuation_node& = delete;
    virtual ~continuation_node() = default;
    virtual void invoke(shared_state& state) = 0;
  };

  explicit shared_state(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept
      : allocator_(allocator) {}

  shared_state(const shared_state&) = delete;
  auto operator=(const shared_state&) -> shared_state& = delete;
  shared_state(shared_state&&) = delete;
  auto operator=(shared_state&&) -> shared_state& = delete;
  ~shared_state() = default;

  void set_value(T value) {
    complete([&] { result_.template emplace<T>(std::move(value)); });
  }

  void set_exception(std::exception_ptr exception) {
    complete([&] { result_.template emplace<std::exception_ptr>(std::move(exception)); });
  }

  // Registers a continuation node (already allocated via allocator()) to
  // run once ready. If already ready, invokes it immediately instead of
  // queueing - single-threaded and synchronous for now; est::loop (M3)
  // will change this to schedule via the ready-queue instead.
  void set_continuation(continuation_node& node) {
    mutex_.lock();
    if (ready()) {
      mutex_.unlock();
      run(node);
      return;
    }
    mutex_.enqueue(node);
    mutex_.unlock();
  }

  [[nodiscard]] auto ready() const noexcept -> bool {
    return !std::holds_alternative<std::monostate>(result_);
  }

  // Retrieves the value, or rethrows the stored exception. Precondition:
  // ready().
  auto get() -> T {
    assert(ready());
    if (auto* exception = std::get_if<std::exception_ptr>(&result_)) {
      std::rethrow_exception(*exception);
    }
    return std::move(std::get<T>(result_));
  }

  [[nodiscard]] auto allocator() const noexcept -> std::pmr::polymorphic_allocator<std::byte> {
    return allocator_;
  }

  void add_ref() noexcept { ++ref_count_; }

  // Decrements the reference count; destroys and deallocates this
  // shared_state (via the allocator it was constructed with) if that
  // was the last reference.
  void release() noexcept {
    if (--ref_count_ == 0) {
      allocator_.delete_object(this);
    }
  }

private:
  template <class F> void complete(F&& store_result) {
    mutex_.lock();
    assert(!ready() && "shared_state completed more than once");
    std::forward<F>(store_result)();
    mutex_.unlock();

    // Drain every registered continuation (normally at most one - future
    // is meant to be a single-consumer handle - but nothing stops a
    // caller from registering more than one via then(), and the
    // underlying list already supports it; LIFO, same order est::mutex's
    // waiter list is documented to use).
    while (true) {
      mutex_.lock();
      auto* waiter = mutex_.dequeue();
      mutex_.unlock();
      if (waiter == nullptr) {
        break;
      }
      run(*static_cast<continuation_node*>(waiter));
    }
  }

  void run(continuation_node& node) {
    node.invoke(*this);
    allocator_.delete_object(&node);
  }

  mutex mutex_;
  std::variant<std::monostate, T, std::exception_ptr> result_;
  int ref_count_ = 0;
  std::pmr::polymorphic_allocator<std::byte> allocator_;
};

// Consumer handle: a thin, move-only view over a shared_state<T>.
// Destroying a future does not destroy the shared_state if something
// else - a still-live est::promise, or (once est::loop exists, M3) the
// loop's own keep-alive registration - still references it.
template <class T> class future {
public:
  explicit future(shared_state<T>* state) noexcept : state_(state) {}
  future(const future&) = delete;
  auto operator=(const future&) -> future& = delete;

  future(future&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

  auto operator=(future&& other) noexcept -> future& {
    if (this != &other) {
      reset();
      state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
  }

  ~future() { reset(); }

  [[nodiscard]] auto ready() const noexcept -> bool { return state_->ready(); }

  auto get() -> T { return state_->get(); }

  // Registers fn to run once ready, as fn(shared_state<T>&) - from which
  // it can call get() (rethrowing any stored exception) or ready(). Runs
  // synchronously, on whichever call stack completes the shared_state
  // (M2 has no loop yet to defer onto - see docs/PLAN.md, M3).
  template <class Fn> void then(Fn&& fn) {
    using node_type = concrete_continuation<std::decay_t<Fn>>;
    auto* node = state_->allocator().template new_object<node_type>(std::forward<Fn>(fn));
    state_->set_continuation(*node);
  }

private:
  template <class Fn>
  class concrete_continuation final : public shared_state<T>::continuation_node {
  public:
    explicit concrete_continuation(Fn fn) : fn_(std::move(fn)) {}
    void invoke(shared_state<T>& state) override { fn_(state); }

  private:
    Fn fn_;
  };

  void reset() noexcept {
    if (state_ != nullptr) {
      state_->release();
      state_ = nullptr;
    }
  }

  shared_state<T>* state_ = nullptr;
};

} // namespace est
