import est;
import std;

#include <catch2/catch_test_macros.hpp>

namespace {

// A minimal derived node, standing in for whatever real type
// (est::detail::ready_node, a future_state<T>'s own continuation_node,
// est::mutex_waiter) actually gets threaded through est::intrusive_list<T>
// in practice - proves dequeue() hands back this exact derived type, not
// the base est::intrusive_list_node, with no cast needed at the call site.
struct tagged_node : est::intrusive_list_node {
  int tag = 0;
};

} // namespace

TEST_CASE("a freshly constructed list is empty", "[intrusive_list]") {
  est::intrusive_list<tagged_node> list;
  REQUIRE(list.empty());
  REQUIRE(list.dequeue() == nullptr);
}

TEST_CASE("enqueue/dequeue is FIFO", "[intrusive_list]") {
  est::intrusive_list<tagged_node> list;
  tagged_node a;
  tagged_node b;
  tagged_node c;

  list.enqueue(a);
  list.enqueue(b);
  list.enqueue(c);
  REQUIRE_FALSE(list.empty());

  REQUIRE(list.dequeue() == &a);
  REQUIRE(list.dequeue() == &b);
  REQUIRE(list.dequeue() == &c);
  REQUIRE(list.dequeue() == nullptr);
  REQUIRE(list.empty());
}

TEST_CASE("a node enqueued after a dequeue that emptied the list is still reachable",
          "[intrusive_list]") {
  // Regression test for the tail_ pointer FIFO needs: dequeuing the last
  // node must reset tail_ back to nullptr, or a later enqueue() would
  // append onto a dangling tail_ instead of correctly becoming the new
  // sole head_.
  est::intrusive_list<tagged_node> list;
  tagged_node a;
  tagged_node b;

  list.enqueue(a);
  REQUIRE(list.dequeue() == &a);
  REQUIRE(list.empty());

  list.enqueue(b);
  REQUIRE(list.dequeue() == &b);
  REQUIRE(list.empty());
}

TEST_CASE("dequeue() returns the derived type directly, no cast needed", "[intrusive_list]") {
  est::intrusive_list<tagged_node> list;
  tagged_node a;
  a.tag = 42;
  list.enqueue(a);

  tagged_node* dequeued = list.dequeue();
  REQUIRE(dequeued != nullptr);
  // Only reachable at all without an explicit cast if dequeue() already
  // returns tagged_node*, not est::intrusive_list_node*.
  REQUIRE(dequeued->tag == 42);
}

TEST_CASE("drain() visits every remaining node in dequeue order and empties the list",
          "[intrusive_list]") {
  est::intrusive_list<tagged_node> list;
  tagged_node a;
  tagged_node b;
  tagged_node c;
  a.tag = 1;
  b.tag = 2;
  c.tag = 3;
  list.enqueue(a);
  list.enqueue(b);
  list.enqueue(c);

  std::vector<int> visited;
  list.drain([&](tagged_node& node) { visited.push_back(node.tag); });

  REQUIRE(visited == std::vector{1, 2, 3});
  REQUIRE(list.empty());
}

TEST_CASE("drain() on an empty list calls fn zero times", "[intrusive_list]") {
  est::intrusive_list<tagged_node> list;
  int calls = 0;
  list.drain([&](tagged_node&) { ++calls; });
  REQUIRE(calls == 0);
}
