export module est:util.shared_ptr;

import std;

export namespace est {

template <class T> class shared_ptr;

} // namespace est

namespace est::detail {

// The reference-counting algorithm both est::shared_ptr<T> specializations
// (below) share: bump on copy, steal-and-null on move, decrement-and-
// free-at-zero on reset. Identical either way - the two specializations
// differ only in *where* a shared object's ref count/allocator/value
// actually live (a separate shared_ptr_control_block<T>, or directly
// inside a T that inherits est::ref_counted), never in the ref-counting
// algorithm itself. Factored out once, here, instead of kept as two
// ~90-line copies that would otherwise have to stay in lockstep by hand
// for logic this central to every node/future_state<T> in the framework.
//
// CRTP, not a runtime interface: there is exactly one object alive per
// shared_ptr<T>, nothing to dispatch on at runtime. `Derived` (always the
// concrete shared_ptr<T> specialization publicly inheriting this) supplies
// three tiny private static functions taking a `Pointer` -
// element_of()/ref_count_of()/destroy() - and this class supplies
// everything built on top of them, once. Those three stay private to
// Derived (not public API, and - for the est::ref_counted-based
// specialization - reaching into T's own private ref_count_/allocator_
// members) rather than free functions: a nested/member function already
// shares its enclosing class's access rights (the same reasoning
// est::mutex::lock_guard or future_state<T>::concrete_continuation<Fn, U>
// already rely on for their own private-member access), so Derived's own
// pre-existing friendship with est::ref_counted (its `template <class>
// friend class shared_ptr;` grant) already covers them - this base only
// needs Derived to friend *it* back (`friend base;`) so it can call them.
//
// Every constructor below is private, plus `friend Derived;` - the same
// "private constructor(s), friend the one type meant to derive from this"
// shape detail::current_allocator_new_delete<Derived>'s own doc comment
// (est:util.current_loop) already uses for an unrelated CRTP mixin in
// this codebase. Without it, nothing would stop unrelated code from
// writing `shared_ptr_common<Foo, Bar, Baz> x;` directly - a type that
// only makes sense paired with the exact Derived/Pointer/T triple one of
// shared_ptr<T>'s own two specializations (below) instantiates it with,
// never spelled out on its own. `ptr_` is private for the same reason,
// not merely protected: Derived (both specializations) only ever reaches
// it indirectly, through this base's own public swap()/reset()/get()/
// etc. and through the constructors below (which `friend Derived;`
// already grants access to) - it never needs to touch `ptr_` by name
// itself.
template <class Derived, class Pointer, class T> class shared_ptr_common {
public:
  // Copy-and-swap: self-assignment-safe without an explicit check (the
  // temporary is a copy of `other`, swapped in, and the old ptr_ - ours,
  // even if `other` aliases *this - is released when the temporary goes
  // out of scope). The static check below doesn't recognize this idiom
  // as self-assignment-safe generically; shared_ptr_tests.cpp exercises
  // self-assignment directly, for both specializations.
  // NOLINTNEXTLINE(bugprone-unhandled-self-assignment)
  auto operator=(const shared_ptr_common& other) noexcept -> shared_ptr_common& {
    shared_ptr_common(other).swap(*this);
    return *this;
  }

  auto operator=(shared_ptr_common&& other) noexcept -> shared_ptr_common& {
    swap(other);
    return *this;
  }

  ~shared_ptr_common() { reset(); }

  void swap(shared_ptr_common& other) noexcept { std::swap(ptr_, other.ptr_); }

  void reset() noexcept {
    if (ptr_ == nullptr) {
      return;
    }
    if (--Derived::ref_count_of(ptr_) == 0) {
      Derived::destroy(ptr_);
    }
    ptr_ = nullptr;
  }

  [[nodiscard]] auto get() const noexcept -> T* {
    return ptr_ != nullptr ? Derived::element_of(ptr_) : nullptr;
  }

  auto operator*() const noexcept -> T& { return *Derived::element_of(ptr_); }
  auto operator->() const noexcept -> T* { return Derived::element_of(ptr_); }

  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  // The number of shared_ptr<T> instances (including *this) currently
  // sharing this object - 0 for an empty (default-constructed or
  // moved-from) shared_ptr, which owns nothing to count. Lets a caller
  // that already holds the last reference to something take a cheaper
  // path than a caller sharing it with others would - see
  // future_state<T>::concrete_continuation<Fn, U>::run() (est:future,
  // issue #64) for the one real use of this today: it's safe to move the
  // stored value out, instead of copying/referencing it, exactly when
  // `count() == 1` on the shared_ptr<future_state<T>> a continuation
  // node holds, since nothing else can be left to observe the
  // moved-from state afterward.
  [[nodiscard]] auto count() const noexcept -> int {
    return ptr_ != nullptr ? Derived::ref_count_of(ptr_) : 0;
  }

private:
  friend Derived;

  shared_ptr_common() noexcept = default;

  shared_ptr_common(const shared_ptr_common& other) noexcept : ptr_(other.ptr_) {
    if (ptr_ != nullptr) {
      ++Derived::ref_count_of(ptr_);
    }
  }

  shared_ptr_common(shared_ptr_common&& other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)) {}

  explicit shared_ptr_common(Pointer ptr) noexcept : ptr_(ptr) {}

  Pointer ptr_ = nullptr;
};

// The primary est::shared_ptr<T> template's storage: combines the ref
// count, the allocator, and T into one allocation (see shared_ptr<T>'s
// own doc comment for why - no separate control-block allocation the way
// std::shared_ptr needs one when not built via make_shared). A free
// struct in this unexported est::detail namespace, not a private nested
// type of shared_ptr<T> itself: shared_ptr<T> needs to name it as
// shared_ptr_common's own Pointer template argument in its base-clause,
// evaluated before any of shared_ptr<T>'s own nested declarations would
// be visible - living here instead gives it exactly the same
// encapsulation (invisible to any `import est;` consumer) with no
// ordering problem.
//
// Deliberately spells out std::pmr::polymorphic_allocator<std::byte>
// directly below rather than introducing a member `using allocator_type
// = ...;` alias the way shared_ptr<T> itself does: std::uses_allocator
// (checked internally by std::pmr::polymorphic_allocator::new_object(),
// which make() below calls) treats any type exposing a nested
// `allocator_type` convertible from the allocator as opting into
// uses-allocator construction - which would silently append a *second*
// allocator argument after `args...` (the "trailing convention"), on
// top of the one this constructor already takes explicitly as
// `allocator_in`, breaking construction for any T whose own constructor
// doesn't also expect a trailing allocator. Naming this differently
// (or not naming it as a type alias at all) keeps this struct invisible
// to that trait, exactly as it was invisible to it before this class
// existed - a private nested struct of shared_ptr<T> can freely reuse
// the name allocator_type from its enclosing scope in its own
// declarations without that making it *this struct's own* nested
// allocator_type member (the outer name is a normal enclosing-scope
// lookup, not something std::uses_allocator's exact-member-name
// detection can see) - a free struct in this namespace has no such
// enclosing scope to borrow the name from, so it has to spell the type
// out to avoid accidentally declaring one.
template <class T> struct shared_ptr_control_block {
  template <class... Args>
  explicit shared_ptr_control_block(std::pmr::polymorphic_allocator<std::byte> allocator_in,
                                    Args&&... args)
      : allocator(allocator_in), value(std::forward<Args>(args)...) {}

  std::pmr::polymorphic_allocator<std::byte> allocator;
  int ref_count = 1;
  T value;
};

} // namespace est::detail

export namespace est {

// Non-template CRTP-style base - deducing this (C++23) stands in for the
// classic template parameter - giving a T its own intrusive reference
// count and allocator. A T that inherits this is allocated *directly* by
// shared_ptr<T>'s partial specialization below, instead of being wrapped
// in a separate control block the way every other T is (see shared_ptr<T>'s
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
  // allocator_ below (including its own private element_of()/
  // ref_count_of()/destroy() statics, est:util.shared_ptr's own
  // detail::shared_ptr_common doc comment), and to the intrusive
  // specialization's private pointer-adopting constructor (needed by
  // shared_from_this() above).
  template <class> friend class shared_ptr;

  allocator_type allocator_;
  int ref_count_ = 1;
};

// A minimal, single-threaded (plain int ref count, no atomics - same
// rationale as every other primitive in this codebase) reference-counted
// pointer, backed by a std::pmr::polymorphic_allocator.
//
// This is the primary template, used for any T that does *not* inherit
// est::ref_counted (above) - see the partial specialization below this
// class for a T that does. The actual copy/move/reset/get/count machinery
// lives in detail::shared_ptr_common, shared with that other
// specialization (see its own doc comment) - this class only adds what's
// genuinely different between the two: make()'s allocation shape (a
// detail::shared_ptr_control_block<T>, combining the ref count,
// allocator, and T in one allocation - no separate control-block
// allocation the way std::shared_ptr needs one when not built via
// make_shared) and the three tiny accessors detail::shared_ptr_common
// calls to reach it.
template <class T>
class shared_ptr
    : public detail::shared_ptr_common<shared_ptr<T>, detail::shared_ptr_control_block<T>*, T> {
  using control_block = detail::shared_ptr_control_block<T>;
  using base = detail::shared_ptr_common<shared_ptr, control_block*, T>;

public:
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

  // Copy/move/destructor are all implicitly generated here - this class
  // adds no members of its own beyond what `base` already carries, so
  // the compiler-generated versions just forward straight to base's own
  // (see detail::shared_ptr_common's own doc comment), with no need to
  // redeclare them.
  shared_ptr() noexcept = default;

  template <class... Args>
  static auto make(allocator_type allocator, Args&&... args) -> shared_ptr {
    auto* control =
        allocator.template new_object<control_block>(allocator, std::forward<Args>(args)...);
    return shared_ptr(control);
  }

private:
  friend base;

  static auto element_of(control_block* control) noexcept -> T* { return &control->value; }
  static auto ref_count_of(control_block* control) noexcept -> int& { return control->ref_count; }
  static void destroy(control_block* control) noexcept {
    auto allocator = control->allocator;
    allocator.delete_object(control);
  }

  explicit shared_ptr(control_block* control) noexcept : base(control) {}
};

// Partial specialization for a T that inherits est::ref_counted (above):
// no separate control block - T carries its own ref count and allocator
// (inherited from ref_counted), so this allocates and frees T directly.
// Same public interface as the primary template above (both inherit it
// from the same detail::shared_ptr_common - see its own doc comment);
// which one applies is resolved entirely at compile time and invisible
// to shared_ptr<T>'s own callers.
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
class shared_ptr<T> : public detail::shared_ptr_common<shared_ptr<T>, T*, T> {
  static_assert(std::is_final_v<T>,
                "a T inheriting est::ref_counted must be marked final - see "
                "this specialization's own doc comment just above");

  using base = detail::shared_ptr_common<shared_ptr, T*, T>;

public:
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

  // Copy/move/destructor are all implicitly generated here - see the
  // primary template's own identical comment just above.
  shared_ptr() noexcept = default;

  template <class... Args>
  static auto make(allocator_type allocator, Args&&... args) -> shared_ptr {
    auto* obj = allocator.template new_object<T>(allocator, std::forward<Args>(args)...);
    return shared_ptr(obj);
  }

private:
  // ref_counted::shared_from_this() is the only other caller of the
  // pointer-adopting constructor below (besides make()) - it bumps
  // ref_count_ itself (a private member of its own base) before handing
  // the raw pointer off, so this constructor deliberately doesn't bump
  // it again.
  friend class ref_counted;
  friend base;

  static auto element_of(T* obj) noexcept -> T* { return obj; }
  static auto ref_count_of(T* obj) noexcept -> int& { return obj->ref_count_; }
  static void destroy(T* obj) noexcept {
    auto allocator = obj->allocator_;
    allocator.delete_object(obj);
  }

  explicit shared_ptr(T* ptr) noexcept : base(ptr) {}
};

} // namespace est
