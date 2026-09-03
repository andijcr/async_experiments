import est;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("lock/unlock toggle locked()", "[mutex]") {
  est::mutex m;

  REQUIRE_FALSE(m.locked());
  m.lock();
  REQUIRE(m.locked());
  m.unlock();
  REQUIRE_FALSE(m.locked());
}

TEST_CASE("waiter list is LIFO and payload-free", "[mutex]") {
  est::mutex m;
  est::mutex_waiter a;
  est::mutex_waiter b;
  est::mutex_waiter c;

  REQUIRE_FALSE(m.has_waiters());

  m.lock();
  m.enqueue(a);
  m.enqueue(b);
  m.enqueue(c);
  m.unlock();

  REQUIRE(m.has_waiters());

  m.lock();
  REQUIRE(m.dequeue() == &c);
  REQUIRE(m.dequeue() == &b);
  REQUIRE(m.dequeue() == &a);
  REQUIRE(m.dequeue() == nullptr);
  m.unlock();

  REQUIRE_FALSE(m.has_waiters());
}
