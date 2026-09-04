import est;

#include <catch2/catch_test_macros.hpp>
#include <memory_resource>
#include <utility>

namespace {

// Wraps the default resource, counting allocate()/deallocate() calls so
// tests can assert every allocation was balanced by a deallocation - the
// only practical way to catch a leaked (or wrongly-sized) control block
// in a unit test without a sanitizer.
class counting_resource : public std::pmr::memory_resource {
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

  [[nodiscard]] auto
  do_is_equal(const std::pmr::memory_resource& other) const noexcept -> bool override {
    return this == &other;
  }
};

} // namespace

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

TEST_CASE("a default-constructed shared_ptr is empty", "[shared_ptr]") {
  est::shared_ptr<int> ptr;
  REQUIRE_FALSE(ptr);
  REQUIRE(ptr.get() == nullptr);
}
