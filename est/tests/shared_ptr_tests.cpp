import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

using std::pmr::memory_resource;

// Wraps the default resource, counting allocate()/deallocate() calls so
// tests can assert every allocation was balanced by a deallocation - the
// only practical way to catch a leaked (or wrongly-sized) control block
// in a unit test without a sanitizer.
class counting_resource : public memory_resource {
public:
  int allocations = 0;
  int deallocations = 0;

private:
  auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
    ++allocations;
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }

  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
    ++deallocations;
    std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
  }

  // Unqualified memory_resource (via the using-declaration above) keeps this
  // signature under the column limit with [[nodiscard]] kept, rather than
  // relying on a return-type line-wrap: clang-format 18 (local) and 22 (CI)
  // disagree on how to wrap this signature when written with the fully
  // qualified std::pmr::memory_resource name.
  [[nodiscard]] auto do_is_equal(const memory_resource& other) const noexcept -> bool override {
    return this == &other;
  }
};

// Opts into est::ref_counted to exercise it directly, independent of its
// one real caller (est::future_state<T>, est:future). Its constructor
// takes the allocator first and forwards it to ref_counted, exactly the
// contract shared_ptr<T>'s intrusive specialization requires (see
// ref_counted's own doc comment, util/shared_ptr.cppm).
class self_aware final : public est::ref_counted {
public:
  self_aware(std::pmr::polymorphic_allocator<std::byte> allocator, int value_in)
      : ref_counted(allocator), value(value_in) {}

  int value;
};

} // namespace

TEST_CASE("ref_counted::shared_from_this() points at the same object", "[shared_ptr]") {
  auto original = est::shared_ptr<self_aware>::make(std::pmr::get_default_resource(), 42);
  auto self = original->shared_from_this();
  REQUIRE(self.get() == original.get());
  REQUIRE(self->value == 42);
}

TEST_CASE("shared_from_this() bumps the ref count like an ordinary copy would", "[shared_ptr]") {
  counting_resource resource;
  auto original = est::shared_ptr<self_aware>::make(&resource, 1);
  {
    auto self = original->shared_from_this();
    original.reset();
    // `self` is the only reference left, but it's still a valid one -
    // shared_from_this() must have bumped the ref count, not just handed
    // back a raw view of an object original.reset() just dropped.
    REQUIRE(resource.deallocations == 0);
    REQUIRE(self->value == 1);
  }
  REQUIRE(resource.deallocations == 1);
}

TEST_CASE("a shared_ptr<T> for a T that doesn't opt into ref_counted uses the primary template",
          "[shared_ptr]") {
  // Guards partial-specialization selection: shared_ptr<T>'s primary
  // (control_block-based) template must be exactly what a non-ref_counted
  // T gets, unconditionally - est::shared_ptr<int> (used throughout the
  // rest of this file) is exactly such a T, and never sees ref_counted's
  // allocator-first construction contract.
  auto ptr = est::shared_ptr<int>::make(std::pmr::get_default_resource(), 7);
  REQUIRE(*ptr == 7);
}

TEST_CASE("a ref_counted T is allocated once, not once for the object and once for a wrapping "
          "control block",
          "[shared_ptr]") {
  counting_resource resource;
  auto ptr = est::shared_ptr<self_aware>::make(&resource, 3);
  REQUIRE(resource.allocations == 1);
  ptr.reset();
  REQUIRE(resource.deallocations == 1);
}

TEST_CASE("make() constructs a value reachable through the pointer", "[shared_ptr]") {
  auto ptr = est::shared_ptr<int>::make(std::pmr::get_default_resource(), 42);
  REQUIRE(ptr);
  REQUIRE(*ptr == 42);
  REQUIRE(*ptr.get() == 42);
}

TEST_CASE("copying bumps the ref count and both handles stay valid", "[shared_ptr]") {
  counting_resource resource;
  {
    auto first = est::shared_ptr<int>::make(&resource, 7);
    auto second = first; // NOLINT(performance-unnecessary-copy-initialization)
    REQUIRE(*first == 7);
    REQUIRE(*second == 7);
    *second = 9;
    REQUIRE(*first == 9); // same control block
  }
  REQUIRE(resource.allocations == 1);
  REQUIRE(resource.deallocations == 1);
}

TEST_CASE("moving leaves the source empty", "[shared_ptr]") {
  auto first = est::shared_ptr<int>::make(std::pmr::get_default_resource(), 3);
  auto second = std::move(first);
  // Deliberately querying a moved-from object's state - well-defined for
  // this type (move leaves control_ null) and exactly what this test
  // means to check.
  REQUIRE_FALSE(first); // NOLINT(bugprone-use-after-move)
  REQUIRE(second);
  REQUIRE(*second == 3);
}

TEST_CASE("the last reference dropping deallocates, not before", "[shared_ptr]") {
  counting_resource resource;
  auto first = est::shared_ptr<int>::make(&resource, 1);
  {
    // The copy itself, not any read of `second`, is what this test
    // exercises - it must be a real second shared_ptr (not a const&) to
    // prove copying bumps the ref count and keeps the control block
    // alive past `second`'s own scope.
    auto second = first; // NOLINT(performance-unnecessary-copy-initialization)
    REQUIRE(resource.deallocations == 0);
  }
  REQUIRE(resource.allocations == 1);
  REQUIRE(resource.deallocations == 0);
  first.reset();
  REQUIRE(resource.deallocations == 1);
}

TEST_CASE("self-copy-assignment and self-move-assignment are safe", "[shared_ptr]") {
  counting_resource resource;
  auto ptr = est::shared_ptr<int>::make(&resource, 5);

  auto& self_ref = ptr;
  ptr = self_ref;
  REQUIRE(ptr);
  REQUIRE(*ptr == 5);

  // std::move(self_ref) rather than std::move(ptr) directly: the latter
  // is flagged by -Wself-move at compile time, but the aliasing (and
  // thus the actual self-move-assignment this exercises at runtime) is
  // identical either way.
  ptr = std::move(self_ref);
  REQUIRE(ptr);
  REQUIRE(*ptr == 5);
  REQUIRE(resource.allocations == 1);
  REQUIRE(resource.deallocations == 0);
}

TEST_CASE("self-copy-assignment and self-move-assignment are safe for a ref_counted T too",
          "[shared_ptr]") {
  // Same exercise as the test above, but for shared_ptr<T>'s intrusive
  // (ref_counted-based) specialization - copy-and-swap is self-assignment-
  // safe there for the identical reason it is on the primary template
  // (see that specialization's own NOLINTNEXTLINE(bugprone-unhandled-
  // self-assignment) comment): the temporary built from `other` bumps
  // ref_count_ before the swap, so a self-assignment's extra increment
  // and the temporary's own decrement on destruction cancel out exactly.
  counting_resource resource;
  auto ptr = est::shared_ptr<self_aware>::make(&resource, 5);

  auto& self_ref = ptr;
  ptr = self_ref;
  REQUIRE(ptr);
  REQUIRE(ptr->value == 5);

  ptr = std::move(self_ref);
  REQUIRE(ptr);
  REQUIRE(ptr->value == 5);
  REQUIRE(resource.allocations == 1);
  REQUIRE(resource.deallocations == 0);
}

TEST_CASE("a default-constructed shared_ptr is empty", "[shared_ptr]") {
  est::shared_ptr<int> ptr;
  REQUIRE_FALSE(ptr);
  REQUIRE(ptr.get() == nullptr);
}

TEST_CASE("count() tracks copies and drops for a non-ref_counted T", "[shared_ptr]") {
  auto first = est::shared_ptr<int>::make(std::pmr::get_default_resource(), 1);
  REQUIRE(first.count() == 1);
  {
    auto second = first; // NOLINT(performance-unnecessary-copy-initialization)
    REQUIRE(first.count() == 2);
    REQUIRE(second.count() == 2);
  }
  REQUIRE(first.count() == 1);
}

TEST_CASE("count() is 0 for an empty or moved-from shared_ptr", "[shared_ptr]") {
  est::shared_ptr<int> empty;
  REQUIRE(empty.count() == 0);

  auto first = est::shared_ptr<int>::make(std::pmr::get_default_resource(), 1);
  auto second = std::move(first);
  REQUIRE(first.count() == 0); // NOLINT(bugprone-use-after-move)
  REQUIRE(second.count() == 1);
}

TEST_CASE("count() tracks copies and drops for a ref_counted T too", "[shared_ptr]") {
  auto first = est::shared_ptr<self_aware>::make(std::pmr::get_default_resource(), 1);
  REQUIRE(first.count() == 1);
  {
    auto second = first; // NOLINT(performance-unnecessary-copy-initialization)
    REQUIRE(first.count() == 2);
    REQUIRE(second.count() == 2);
  }
  REQUIRE(first.count() == 1);

  auto self = first->shared_from_this();
  REQUIRE(first.count() == 2);
  REQUIRE(self.count() == 2);
}
