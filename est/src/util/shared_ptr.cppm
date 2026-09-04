export module est:util.shared_ptr;

import std;

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
    return shared_ptr(control);
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

} // namespace est
