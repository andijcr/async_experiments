module;

#include <cassert>
#include <exception>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include <variant>

export module est:future;

import :sync.mutex;
import :util.scope_exit;

export namespace est {

template <class T> class shared_state;

} // namespace est

namespace est::detail {

// Type-agnostic base for a shared_state<T>'s queued continuations: only
// the parts that don't depend on T live here, so they're compiled once
// instead of once per T. shared_state<T>'s own destructor (which only
// ever needs to destroy(), never invoke()) operates on this directly.
class waiter_node : public mutex_waiter {
public:
  waiter_node() = default;
  waiter_node(const waiter_node&) = delete;
  auto operator=(const waiter_node&) -> waiter_node& = delete;
  waiter_node(waiter_node&&) = delete;
  auto operator=(waiter_node&&) -> waiter_node& = delete;
  virtual ~waiter_node() = default;

  // Deallocates *this through the *actual* allocated type (each
  // override does `allocator.delete_object(this)` with `this` typed as
  // the concrete class). Calling `allocator.delete_object` on a
  // waiter_node& directly would deduce the base type and deallocate
  // with the base's size/alignment instead of the derived type actually
  // allocated - undefined behaviour per memory_resource::deallocate's
  // precondition that the size/alignment match the original allocate()
  // call, silently "working" with the default new/delete resource but
  // corrupting a pool-style resource.
  virtual void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept = 0;
};

// The T-dependent half of a queued continuation: adds only the one
// thing that actually needs T (invoke()). Not nested inside
// shared_state<T> - it's an implementation detail of :future, not part
// of shared_state's public surface, so it lives here instead.
template <class T> class continuation_node : public waiter_node {
public:
  virtual void invoke(shared_state<T>& state) = 0;
};

} // namespace est::detail

export namespace est {

// The single owned object behind an est::promise<T>/est::future<T> pair
// - a view over shared state, not the state itself (docs/PLAN.md). Holds
// value-or-exception storage and a continuation slot built on
// est::waiter_list, plus a plain (non-atomic - single-threaded, see
// est::mutex's own docs) reference count. Always heap-allocated via a
// std::pmr::polymorphic_allocator (see make_promise_future() in
// est:promise) - never constructed directly by a caller.
template <class T> class shared_state {
public:
  using continuation_node = detail::continuation_node<T>;

  explicit shared_state(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept
      : allocator_(allocator) {}

  shared_state(const shared_state&) = delete;
  auto operator=(const shared_state&) -> shared_state& = delete;
  shared_state(shared_state&&) = delete;
  auto operator=(shared_state&&) -> shared_state& = delete;

  // Destroys (without invoking) any continuation still queued: either the
  // promise was dropped without ever completing, or complete()'s drain
  // loop was aborted partway through by a throwing continuation (see
  // run()'s comment). Without this, those already-allocated nodes are
  // simply unreachable once this shared_state itself is gone - a
  // permanent leak, not just a skipped notification.
  ~shared_state() {
    while (auto* waiter = waiters_.dequeue()) {
      // Safe by construction, not by RTTI: every waiter ever enqueued
      // into waiters_ is a detail::waiter_node (set_continuation() only
      // accepts a continuation_node&, itself a waiter_node) - there is
      // no dynamic_cast alternative worth paying for here.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
      static_cast<detail::waiter_node*>(waiter)->destroy(allocator_);
    }
  }

  void set_value(const T& value) {
    complete([&] { result_.template emplace<T>(value); });
  }

  void set_value(T&& value) {
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
    if (ready()) {
      run(node);
      return;
    }
    waiters_.enqueue(node);
  }

  [[nodiscard]] auto ready() const noexcept -> bool {
    return !std::holds_alternative<std::monostate>(result_);
  }

  // Retrieves the value, or rethrows the stored exception. Precondition:
  // ready(). Deducing this: called on an lvalue (or const lvalue), this
  // returns const T& - non-consuming, safe for the multiple registered
  // then() continuations that each read it without consuming it. Called
  // on an rvalue (std::move(state).get()), this returns T&&, an explicit
  // opt-in to move from the stored value - same caveat as
  // std::optional<T>::value() &&: moving from a value something else
  // (another queued continuation, or a later then()) still needs is the
  // caller's mistake to avoid, not something this class defends against.
  template <class Self>
  [[nodiscard]] auto
  get(this Self&& self) -> std::conditional_t<std::is_lvalue_reference_v<Self>, const T&, T&&> {
    assert(self.ready());
    if (auto* exception = std::get_if<std::exception_ptr>(&self.result_)) {
      std::rethrow_exception(*exception);
    }
    // std::variant::get()'s own overload set (on variant&/const
    // variant&/variant&&/const variant&&) picks the right return
    // category from the forwarded expression - no manual branching
    // needed on top of it.
    return std::get<T>(std::forward<Self>(self).result_);
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
    // Precondition, not a recoverable error: set_value()/set_exception()
    // must each be called at most once. Debug-only (unlike
    // std::promise, which throws) - a release build that violates this
    // silently overwrites result_ and, for any continuations that already
    // ran off the first completion, delivers a value/exception they never
    // see. Revisit if that turns out to matter in practice; not changing
    // it speculatively now.
    assert(!ready() && "shared_state completed more than once");
    std::forward<F>(store_result)();

    // Drain every registered continuation (normally at most one - future
    // is meant to be a single-consumer handle - but nothing stops a
    // caller from registering more than one via then(), and the
    // underlying list already supports it; LIFO, same order
    // est::waiter_list is documented to use).
    while (auto* waiter = waiters_.dequeue()) {
      run(*static_cast<continuation_node*>(waiter));
    }
  }

  // Guarantees the node is destroyed even if invoke() throws (a
  // continuation's own exception is not this shared_state's problem to
  // swallow, but leaking the node it ran in would be a separate bug on
  // top of whatever the continuation did). A throwing continuation does
  // still abort the drain loop in complete() before any later-queued
  // continuations run; that's an M2-scope limitation, to be revisited
  // once M3's loop dispatches continuations independently instead of
  // inline on the completer's own call stack.
  void run(continuation_node& node) {
    scope_exit const guard{[&node, allocator = allocator_] { node.destroy(allocator); }};
    node.invoke(*this);
  }

  waiter_list waiters_;
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
    std::swap(state_, other.state_);
    return *this;
  }

  ~future() { reset(); }

  [[nodiscard]] auto ready() const noexcept -> bool { return state_->ready(); }

  // Deducing this: future.get() (lvalue) copy-constructs from the
  // shared_state's non-consuming get(); std::move(future).get() forwards
  // its rvalue-ness through, so the shared_state's value is moved
  // directly into the return value instead of copied - safe here
  // specifically because a future is a single-consumer handle, unlike
  // shared_state<T>::get() itself, which multiple then() continuations
  // may each call.
  //
  // self isn't itself forwarded (NOLINTNEXTLINE below): state_ is a raw
  // pointer member, and forwarding a pointer doesn't propagate value
  // category to what it points to the way it would for a value/reference
  // member - the if constexpr branch is what actually turns *state_'s
  // lvalue-ness into an xvalue for the rvalue-self case.
  // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
  template <class Self> [[nodiscard]] auto get(this Self&& self) -> T {
    if constexpr (std::is_lvalue_reference_v<Self>) {
      return self.state_->get();
    } else {
      return std::move(*self.state_).get();
    }
  }

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

    // `this` here is concrete_continuation<Fn>*, so delete_object
    // deallocates with this type's actual size/alignment - the whole
    // reason destroy() is virtual instead of the caller deallocating
    // through a waiter_node& (see that class's own doc comment).
    void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept override {
      allocator.delete_object(this);
    }

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
