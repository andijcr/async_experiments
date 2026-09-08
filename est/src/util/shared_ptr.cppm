export module est:util.shared_ptr;

import std;
import :check;

export namespace est {

// A minimal, single-threaded (plain int ref count, no atomics - same
// rationale as every other primitive in this codebase) reference-counted
// pointer, backed by a std::pmr::polymorphic_allocator. Combines the ref
// count, the allocator, and the T into one control block allocated in a
// single call (no separate control-block allocation the way
// std::shared_ptr needs one when not built via make_shared).
template <class T> class shared_ptr {
public:
  // Named (rather than spelled out at each use below) partly for
  // convention, partly so signatures using it stay short enough to
  // sidestep clang-format version-specific line-wrap disagreements (see
  // docs/PLAN.md's note on this happening more than once already for a
  // similarly-shaped signature).
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

  shared_ptr() noexcept = default;

  template <class... Args>
  static auto make(allocator_type allocator, Args&&... args) -> shared_ptr {
    auto* control =
        allocator.template new_object<control_block>(allocator, std::forward<Args>(args)...);
    shared_ptr result(control);
    // Wires up T's back-pointer to its own control block, for a T that
    // opts in by inheriting from est::enable_shared_from_this<T> - a
    // no-op (the requires-expression is simply false) for every other T.
    // Must happen after construction (the constructor above already ran
    // as part of allocating control), same reason
    // std::enable_shared_from_this needs the same two-step wiring: T
    // can't know its own control block from inside its own constructor.
    if constexpr (requires(T& value, void* block) { value.set_owning_control_block(block); }) {
      control->value.set_owning_control_block(control);
    }
    return result;
  }

  shared_ptr(const shared_ptr& other) noexcept : control_(other.control_) {
    if (control_ != nullptr) {
      ++control_->ref_count;
    }
  }

  // Copy-and-swap: self-assignment-safe without an explicit check (the
  // temporary is a copy of `other`, swapped in, and the old control_ -
  // ours, even if `other` aliases *this - is released when the
  // temporary goes out of scope). The static check below doesn't
  // recognize this idiom as self-assignment-safe generically; a
  // dedicated test (shared_ptr_tests.cpp) exercises self-assignment
  // directly.
  // NOLINTNEXTLINE(bugprone-unhandled-self-assignment)
  auto operator=(const shared_ptr& other) noexcept -> shared_ptr& {
    shared_ptr(other).swap(*this);
    return *this;
  }

  shared_ptr(shared_ptr&& other) noexcept : control_(std::exchange(other.control_, nullptr)) {}

  auto operator=(shared_ptr&& other) noexcept -> shared_ptr& {
    swap(other);
    return *this;
  }

  ~shared_ptr() { reset(); }

  void swap(shared_ptr& other) noexcept { std::swap(control_, other.control_); }

  void reset() noexcept {
    if (control_ == nullptr) {
      return;
    }
    if (--control_->ref_count == 0) {
      auto allocator = control_->allocator;
      allocator.delete_object(control_);
    }
    control_ = nullptr;
  }

  [[nodiscard]] auto get() const noexcept -> T* {
    return control_ != nullptr ? &control_->value : nullptr;
  }

  auto operator*() const noexcept -> T& { return control_->value; }
  auto operator->() const noexcept -> T* { return &control_->value; }

  explicit operator bool() const noexcept { return control_ != nullptr; }

  // Rebuilds a shared_ptr from a control block already known to have at
  // least one live owner, bumping the ref count exactly like a copy
  // would - the mechanism est::enable_shared_from_this<T> (below) uses
  // to hand out shared ownership of T from inside T's own member
  // functions, without T needing to store its own shared_ptr<T> (which
  // would be a self-referential cycle this ref-counted pointer's plain
  // int count could never break: the object would then always hold at
  // least one reference to itself). Not for general use beyond that -
  // nothing here can verify an arbitrary void* actually still points at
  // a live control_block of this T; enable_shared_from_this<T> only ever
  // passes back a pointer make() itself set.
  [[nodiscard]] static auto from_owning_control_block(void* control) noexcept -> shared_ptr {
    auto* typed = static_cast<control_block*>(control);
    ++typed->ref_count;
    return shared_ptr(typed);
  }

private:
  struct control_block {
    template <class... Args>
    explicit control_block(allocator_type allocator_in, Args&&... args)
        : allocator(allocator_in), value(std::forward<Args>(args)...) {}

    allocator_type allocator;
    int ref_count = 1;
    T value;
  };

  explicit shared_ptr(control_block* control) noexcept : control_(control) {}

  control_block* control_ = nullptr;
};

// Opt-in CRTP base giving a shared_ptr<T>-managed T the ability to hand
// out new shared ownership of itself (shared_from_this()) from inside
// its own member functions - the same problem
// std::enable_shared_from_this solves for std::shared_ptr, needed here
// because est::future_state<T> (est:future) invokes a then() callback
// that wants a real est::future<T> - itself just a shared_ptr<T> plus a
// thin interface - without future_state<T> otherwise having any way to
// produce one. shared_ptr<T>::make() wires the back-pointer in
// automatically for any T that inherits from this (see the `if
// constexpr (requires ...)` there); never call set_owning_control_block
// directly otherwise. Weak, not owning: stores a raw pointer, never
// bumps the ref count itself - only shared_from_this() does, exactly
// like any other shared_ptr copy.
template <class T> class enable_shared_from_this {
public:
  // Copy/move stay deleted (rather than just never declared): T is
  // always accessed through shared_ptr<T>, never copied/moved as a
  // value, and a deleted declaration is public by convention (modernize-
  // use-equals-delete) so misuse fails with a clear "deleted function"
  // diagnostic instead of "private member" - unlike the default
  // constructor below, deleting these reveals nothing about the CRTP
  // access restriction that constructor exists to enforce.
  enable_shared_from_this(const enable_shared_from_this&) = delete;
  auto operator=(const enable_shared_from_this&) -> enable_shared_from_this& = delete;
  enable_shared_from_this(enable_shared_from_this&&) = delete;
  auto operator=(enable_shared_from_this&&) -> enable_shared_from_this& = delete;
  // See the matching NOLINT in util/intrusive_list.cppm - same
  // clang-tidy limitation, reproduced here for
  // enable_shared_from_this<future_state<T>> instead.
  // NOLINTNEXTLINE(performance-trivially-destructible)
  ~enable_shared_from_this() = default;

  // Precondition: this object was actually constructed via
  // shared_ptr<T>::make() (which is what sets control_block_ - see
  // set_owning_control_block() below). Debug-checked, unlike the rest of
  // this file's own bare-pointer operations (dereferencing an empty
  // shared_ptr, say) - those match std::shared_ptr's own long-documented
  // "caller's mistake" contract, but building a T that inherits this and
  // then stack- or new-allocating it directly instead of going through
  // make() is a much less obvious way to reach the same null-pointer
  // misuse, specific to this project's own primitive rather than
  // something every shared_ptr user already knows to avoid.
  [[nodiscard]] auto shared_from_this() -> shared_ptr<T> {
    check(control_block_ != nullptr,
          "shared_from_this() called on a T never constructed via shared_ptr<T>::make()");
    return shared_ptr<T>::from_owning_control_block(control_block_);
  }

private:
  // Constructible only by T itself (the one correct CRTP usage - `class
  // Foo : enable_shared_from_this<Bar>` would otherwise compile and
  // silently do the wrong thing) and by shared_ptr<T>, which needs to
  // call set_owning_control_block() below.
  friend T;
  friend class shared_ptr<T>;
  enable_shared_from_this() = default;

  void set_owning_control_block(void* control) noexcept { control_block_ = control; }

  void* control_block_ = nullptr;
};

} // namespace est
