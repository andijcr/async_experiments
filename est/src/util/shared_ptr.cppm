export module est:util.shared_ptr;

import std;

export namespace est {

template <class T> class shared_ptr;

// Non-template CRTP-style base - deducing this (C++23) stands in for the
// classic template parameter - giving a T its own intrusive reference
// count and allocator. A T that inherits this is allocated *directly* by
// shared_ptr<T>'s partial specialization below, instead of being wrapped
// in a separate control_block the way every other T is (see shared_ptr<T>'s
// own doc comment) - and gets shared_from_this() nearly for free, since
// the ref count already lives inside T itself: bump it, hand back a
// shared_ptr<T> aliasing `this`, no separate block to recover the address
// of at all.
//
// Not a template: deducing this lets shared_from_this() deduce Self (the
// actual derived T) straight from the call site, removing the need for
// this class to carry T as a template parameter - and with it, the classic
// CRTP mismatch footgun ("class Foo : enable_shared_from_this<Bar>"
// compiling and silently doing the wrong thing): there's no T to get
// wrong here, since Self is always whatever the caller actually invoked
// shared_from_this() on.
//
// T must be `final`: the destructor below is deliberately non-virtual
// (nothing ever destroys through a ref_counted*, only ever through the
// T* shared_ptr<T>'s own intrusive specialization allocated - see that
// class's own doc comment), so a further-derived class would be
// destroyed through T's destructor instead of its own the moment its ref
// count reached zero. shared_ptr<T>'s intrusive specialization
// static_asserts this for every T that inherits ref_counted.
class ref_counted {
public:
  ref_counted(const ref_counted&) = delete;
  auto operator=(const ref_counted&) -> ref_counted& = delete;
  ref_counted(ref_counted&&) = delete;
  auto operator=(ref_counted&&) -> ref_counted& = delete;

  // Precondition: self was actually constructed via shared_ptr<Self>::
  // make() - see this class's own doc comment. Can't be debug-checked
  // here: ref_count_ is always 1 immediately after construction, so a
  // bad call can't be told apart from a good one just by looking at it.
  // Calling this on a T that wasn't shared_ptr<T>::make()-allocated is
  // undefined behavior the moment the returned shared_ptr's ref count
  // reaches zero and tries to free memory this allocator never allocated
  // in the first place - the same contract std::enable_shared_from_this
  // has.
  template <class Self> [[nodiscard]] auto shared_from_this(this Self& self) -> shared_ptr<Self> {
    ++self.ref_count_;
    return shared_ptr<Self>(&self);
  }

protected:
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

  explicit ref_counted(allocator_type allocator) noexcept : allocator_(allocator) {}
  // Protected and non-virtual: only ever destroyed through shared_ptr<T>'s
  // intrusive specialization, which already knows T's exact (most-derived)
  // type and deletes through T*, never through ref_counted* - the same
  // "type-erased base doesn't need a virtual destructor because nothing
  // ever destroys through it directly" reasoning est::detail::ready_node
  // documents (est:loop) for the identical shape of design.
  ~ref_counted() = default;

private:
  // Grants every shared_ptr<T> instantiation access to ref_count_/
  // allocator_ below, and to the intrusive specialization's private
  // pointer-adopting constructor (needed by shared_from_this() above).
  template <class> friend class shared_ptr;

  allocator_type allocator_;
  int ref_count_ = 1;
};

// A minimal, single-threaded (plain int ref count, no atomics - same
// rationale as every other primitive in this codebase) reference-counted
// pointer, backed by a std::pmr::polymorphic_allocator. Combines the ref
// count, the allocator, and the T into one control block allocated in a
// single call (no separate control-block allocation the way
// std::shared_ptr needs one when not built via make_shared).
//
// This is the primary template, used for any T that does *not* inherit
// est::ref_counted (above) - see the partial specialization below this
// class for a T that does.
template <class T> class shared_ptr {
public:
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

// Partial specialization for a T that inherits est::ref_counted (above):
// no separate control_block - T carries its own ref count and allocator
// (inherited from ref_counted), so this allocates and frees T directly.
// Same public interface as the primary template above; which one applies
// is resolved entirely at compile time and invisible to shared_ptr<T>'s
// own callers.
//
// A T opting into this must accept `allocator_type` as its own
// constructor's first parameter and forward it straight to ref_counted's
// constructor - make() below always passes it as T's first argument. See
// est::future_state<T> (est:future) for a real example. T must also be
// marked `final` (the static_assert below): reset() always deletes
// through a T* it constructed itself, never through a ref_counted*, so a
// further-derived class would silently skip its own destructor -
// exactly the "delete through a base pointer with no virtual destructor"
// bug this design otherwise avoids by never deleting through a base
// pointer at all.
template <class T>
  requires std::derived_from<T, ref_counted>
class shared_ptr<T> {
  static_assert(std::is_final_v<T>,
                "a T inheriting est::ref_counted must be marked final - see "
                "this specialization's own doc comment just above");

public:
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

  shared_ptr() noexcept = default;

  template <class... Args>
  static auto make(allocator_type allocator, Args&&... args) -> shared_ptr {
    auto* obj = allocator.template new_object<T>(allocator, std::forward<Args>(args)...);
    return shared_ptr(obj);
  }

  shared_ptr(const shared_ptr& other) noexcept : ptr_(other.ptr_) {
    if (ptr_ != nullptr) {
      ++ptr_->ref_count_;
    }
  }

  // Copy-and-swap: self-assignment-safe without an explicit check, same
  // reasoning (and the same static-analysis limitation) as the primary
  // template's own operator= above - see its own comment. Exercised
  // directly by shared_ptr_tests.cpp's "self-copy-assignment and
  // self-move-assignment are safe for a ref_counted T too".
  // NOLINTNEXTLINE(bugprone-unhandled-self-assignment)
  auto operator=(const shared_ptr& other) noexcept -> shared_ptr& {
    shared_ptr(other).swap(*this);
    return *this;
  }

  shared_ptr(shared_ptr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

  auto operator=(shared_ptr&& other) noexcept -> shared_ptr& {
    swap(other);
    return *this;
  }

  ~shared_ptr() { reset(); }

  void swap(shared_ptr& other) noexcept { std::swap(ptr_, other.ptr_); }

  void reset() noexcept {
    if (ptr_ == nullptr) {
      return;
    }
    if (--ptr_->ref_count_ == 0) {
      auto allocator = ptr_->allocator_;
      allocator.delete_object(ptr_);
    }
    ptr_ = nullptr;
  }

  [[nodiscard]] auto get() const noexcept -> T* { return ptr_; }

  auto operator*() const noexcept -> T& { return *ptr_; }
  auto operator->() const noexcept -> T* { return ptr_; }

  explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
  // ref_counted::shared_from_this() is the only caller of the
  // pointer-adopting constructor below - it bumps ref_count_ itself
  // (a private member of its own base) before handing the raw pointer
  // off, so this constructor deliberately doesn't bump it again.
  friend class ref_counted;

  explicit shared_ptr(T* ptr) noexcept : ptr_(ptr) {}

  T* ptr_ = nullptr;
};

} // namespace est
