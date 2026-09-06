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

// A minimal, singly-linked, FIFO intrusive list - the exact structure
// est::mutex, est::future_state<T>, and est::loop each separately needed
// for their own waiter/ready-work queues, extracted here once instead of
// reimplemented three times. FIFO (not LIFO, this class's original
// policy - review discussion on issue #45's yield_execution()): a node
// that enqueues itself onto a list it doesn't own (est::loop::ready_,
// concretely) needs a predictable answer to "does whatever was already
// queued run before or after me," and LIFO's answer - "after, I cut to
// the front" - is exactly backwards for that, silently, for any caller
// who assumed otherwise. Concretely a bug waiting to happen: a bespoke
// est::loop::enqueue_ready() node meant to "let other ready work go
// first" would instead run *first* under the old LIFO policy. FIFO gives
// every current user a more conventional guarantee for free - est::mutex's
// waiters are now handed the lock in first-come-first-served order rather
// than most-recently-queued-first, matching what most callers of a mutex
// would assume without reading this file - not just the one caller that
// exposed the mismatch.
//
// A singly-linked list gets O(1) enqueue *and* O(1) dequeue under FIFO
// the same way it did under LIFO, just with one more pointer to maintain -
// no second link field, no doubly-linked list needed. `sentinel_` (repo
// owner's own suggestion, PR #49 review) is what keeps that second
// pointer branch-free: `tail_` always points at *some* real node's `next`
// slot to append onto - either the last enqueued node's, or, when the
// list is empty, the sentinel's own - so enqueue() never needs to ask
// "is this the first node" before deciding what to update. The sentinel
// itself is never a real element (nothing ever enqueue()s it, and
// dequeue() only ever looks at what `sentinel_.next` points *to*, never
// treats the sentinel as a return value).
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

  // Dequeues every remaining node and calls fn(T&) on each, in this
  // list's own dequeue order (FIFO) - the "walk whatever's left and act
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
  intrusive_list_node sentinel_;
  intrusive_list_node* tail_ = &sentinel_;
};

} // namespace est
