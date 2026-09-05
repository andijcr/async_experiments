export module est:util.intrusive_list;

import std;

export namespace est {

// A minimal intrusive singly-linked list node: just the link field,
// nothing else. Anything meant to be threaded through est::intrusive_list<T>
// needs to inherit this (directly or transitively) - est::mutex's own
// waiting parties, est::future_state<T>'s pending continuations, and
// est::loop's ready-queue entries all do, without any of those needing to
// know about each other or about mutexes at all. The name reflects what
// this actually is, not who first needed it - it used to be named
// mutex_waiter and live in est:sync.mutex, back when est::mutex was its
// only user.
class intrusive_list_node {
public:
  intrusive_list_node* next = nullptr;
};

// A minimal, singly-linked, LIFO intrusive list - the exact structure
// est::mutex, est::future_state<T>, and est::loop each separately needed
// for their own waiter/ready-work queues, extracted here once instead of
// reimplemented three times. LIFO because nothing built on any of those
// queues needs FIFO fairness among their entries; a singly-linked list
// makes LIFO the free direction (push and pop both happen at the head).
//
// Templated on T (constrained to derive from intrusive_list_node) rather
// than being a fixed intrusive_list_node-typed list: every current user
// immediately downcasts dequeue()'s result straight back to whatever T
// actually is (mutex_waiter itself for est::mutex; a further-derived type,
// est::detail::ready_node or a future_state<T>'s own continuation_node,
// for est::loop/est::future) - so this list does that cast once, here,
// safe by construction (every node it ever hands back came in through
// enqueue(T&)), instead of every caller repeating the cast - and the
// "safe by construction, not by RTTI" justification that comes with it -
// at each call site.
template <class T>
  requires std::derived_from<T, intrusive_list_node>
class intrusive_list {
public:
  void enqueue(T& node) noexcept {
    node.next = head_;
    head_ = &node;
  }

  [[nodiscard]] auto dequeue() noexcept -> T* {
    auto* head = head_;
    if (head != nullptr) {
      head_ = head->next;
      head->next = nullptr;
    }
    // Safe by construction, not by RTTI: every node ever linked into
    // head_ arrived through enqueue(T&) above, so it's always actually a
    // T - there is no dynamic_cast alternative worth paying for here.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    return static_cast<T*>(head);
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return head_ == nullptr; }

  // Dequeues every remaining node and calls fn(T&) on each, in this
  // list's own dequeue order (LIFO) - the "walk whatever's left and act
  // on it, then it's gone" shape est::loop's and est::future_state<T>'s
  // own destructors (destroying an abandoned node) and
  // est::future_state<T>::complete() (handing every pending continuation
  // off to est::loop) all need, extracted here so it's written once
  // instead of three times. Not for a caller that needs to stop partway
  // through a drain - est::loop's own drain_ready() does, to honor
  // stop() mid-drain, so it still calls dequeue() directly in its own
  // loop instead of using this.
  template <class Fn> void drain(Fn fn) {
    while (auto* node = dequeue()) {
      fn(*node);
    }
  }

private:
  intrusive_list_node* head_ = nullptr;
};

} // namespace est
