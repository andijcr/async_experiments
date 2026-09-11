export module est:util.intrusive_list;

import std;

export namespace est {

// A minimal intrusive singly-linked list node: just the link field,
// nothing else. Anything meant to be threaded through est::intrusive_list<T>
// needs to inherit this (directly or transitively) - est::mutex's own
// waiting parties, est::future_state<T>'s pending continuations, and
// est::loop's ready-queue entries all do, without any of those needing to
// know about each other or about mutexes at all.
class intrusive_list_node {
public:
  intrusive_list_node* next = nullptr;
};

// A minimal, singly-linked, FIFO intrusive list - the shared structure
// behind est::mutex's waiters, est::future_state<T>'s pending
// continuations, and est::loop's ready queue. FIFO matters here: a node
// that enqueues itself onto a list it doesn't own needs a predictable
// answer to "does whatever was already queued run before or after me" -
// LIFO would silently run a newly-queued node first, which is backwards
// for a ready-work or waiter queue. FIFO gives every user a conventional
// guarantee: est::mutex's waiters are handed the lock in
// first-come-first-served order.
//
// A singly-linked list gets O(1) enqueue *and* O(1) dequeue under FIFO,
// with one more pointer to maintain than a stack needs - no second link
// field, no doubly-linked list. `sentinel_` keeps that branch-free:
// `tail_` always points at some real node's `next` slot to append onto -
// either the last enqueued node's, or, when the list is empty, the
// sentinel's own - so enqueue() never needs to ask "is this the first
// node." The sentinel itself is never a real element: nothing ever
// enqueue()s it, and dequeue() only ever looks at what `sentinel_.next`
// points *to*, never treats the sentinel as a return value.
//
// Templated on T (constrained to derive from intrusive_list_node) so
// dequeue() downcasts to the caller's actual node type once, here, safe
// by construction (every node it hands back came in through
// enqueue(T&)), instead of every caller repeating the cast.
template <class T>
  requires std::derived_from<T, intrusive_list_node>
class intrusive_list {
public:
  intrusive_list() = default;
  intrusive_list(const intrusive_list&) = delete;
  auto operator=(const intrusive_list&) -> intrusive_list& = delete;
  intrusive_list(intrusive_list&&) = delete;
  auto operator=(intrusive_list&&) -> intrusive_list& = delete;
  // Already =default, but clang-tidy still flags it for some T (e.g.
  // continuation_node<std::string>) - a module-instantiation-specific
  // false positive.
  // NOLINTNEXTLINE(performance-trivially-destructible)
  ~intrusive_list() = default;

  void enqueue(T& node) noexcept {
    node.next = nullptr;
    tail_->next = &node;
    tail_ = &node;
  }

  [[nodiscard]] auto dequeue() noexcept -> T* {
    auto* head = sentinel_.next;
    if (head != nullptr) {
      sentinel_.next = head->next;
      if (tail_ == head) {
        tail_ = &sentinel_;
      }
      head->next = nullptr;
    }
    // Safe by construction, not by RTTI: every node ever linked into
    // sentinel_.next arrived through enqueue(T&) above, so it's always
    // actually a T (never the sentinel itself) - there is no
    // dynamic_cast alternative worth paying for here.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    return static_cast<T*>(head);
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return sentinel_.next == nullptr; }

  // Dequeues every remaining node and calls fn(T&) on each, in FIFO
  // order. Not for a caller that needs to stop partway through a drain -
  // est::loop's own drain_ready() does (to honor stop() mid-drain), so
  // it calls dequeue() directly instead of using this.
  template <class Fn> void drain(Fn fn) {
    while (auto* node = dequeue()) {
      fn(*node);
    }
  }

private:
  intrusive_list_node sentinel_;
  intrusive_list_node* tail_ = &sentinel_;
};

} // namespace est
