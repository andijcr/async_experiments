export module est:loop;

import std;
import :check;
import :platform;
import :timer;
import :util.intrusive_list;
import :util.scope_exit;

// Type-erased primitives est::loop's ready-queue and pending-timer list
// hold, kept fully independent of est::future/est::promise: :loop needs a
// type-erased "thing to run" for its ready-queue, and est::future's own
// continuation nodes are exactly that - but future_state<T> also needs to
// depend on est::loop to defer onto it, so :loop cannot import :future
// without a circular module dependency (docs/PLAN.md, M3). Resolved by
// keeping :loop the lower-level partition: :future's continuation_node<T>
// inherits ready_node directly instead of :loop naming future_state<T>,
// and est::sleep_for()/sleep_until() (the future<void>-returning sugar
// built on schedule_timer() below) live in :promise instead of here.
// Deliberately not exported, same reasoning as :future's own (now former)
// waiter_node: not part of this partition's public surface, only visible
// to another partition that imports :loop.
namespace est::detail {

// One entry in est::loop's ready-queue: something already known to be
// ready to run, whatever produced it - a fulfilled future_state<T>'s
// continuation today, a resumed coroutine handle eventually (M4).
class ready_node : public intrusive_list_node {
public:
  ready_node() = default;
  ready_node(const ready_node&) = delete;
  auto operator=(const ready_node&) -> ready_node& = delete;
  ready_node(ready_node&&) = delete;
  auto operator=(ready_node&&) -> ready_node& = delete;
  virtual ~ready_node() = default;

  virtual void run() = 0;

  // Deallocates *this through the actual allocated (derived) type - same
  // reasoning as :future's own former waiter_node::destroy(): deducing
  // through this base's size/alignment instead would be undefined
  // behaviour per memory_resource::deallocate's contract.
  virtual void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept = 0;
};

// One entry in est::loop's pending-timer list: something to run once a
// deadline passes. Not itself part of any intrusive list (unlike
// ready_node) - est::loop tracks pending timer_nodes in its own
// pending_timers_ vector, keyed by est::timer_queue's own id, since
// nothing needs to walk them in deadline order beyond what timer_queue's
// min-heap already provides.
class timer_node {
public:
  timer_node() = default;
  timer_node(const timer_node&) = delete;
  auto operator=(const timer_node&) -> timer_node& = delete;
  timer_node(timer_node&&) = delete;
  auto operator=(timer_node&&) -> timer_node& = delete;
  virtual ~timer_node() = default;

  virtual void fire() = 0;
  virtual void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept = 0;
};

} // namespace est::detail

export namespace est {

// The single-threaded run loop M3 adds (docs/PLAN.md): owns a ready-queue
// of already-fulfilled continuations and the M1 timer_queue, and is the
// thing that actually resumes continuations once a promise is fulfilled
// or a timer fires - est::future_state<T>::complete() (est:future) defers
// to it via enqueue_ready() instead of invoking a continuation inline on
// the fulfilling call stack the way M2 did.
//
// An explicit object a caller constructs and threads through
// make_promise_future<T>(loop&) (est:promise), not a global singleton
// like est::platform::instance(): unlike platform (a stateless vtable
// swap), a loop carries real mutable state (the ready-queue, pending
// timers) a shared global would accumulate cross-test contamination in -
// every test that needs one constructs its own.
//
// Precondition: a loop must outlive every future_state<T> (and therefore
// every promise<T>/future<T>/then()-chain) built against it via
// make_promise_future() - est:future's future_state<T> holds a bare
// loop&, not shared ownership, and there is no way to check a dangling
// reference at runtime. See future_state<T>'s own doc comment (est:future)
// for the same precondition from that side.
class loop {
public:
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;
  using clock = std::chrono::steady_clock;

  explicit loop(allocator_type allocator = {})
      : allocator_(allocator), timers_(allocator), pending_timers_(allocator) {}
  loop(const loop&) = delete;
  auto operator=(const loop&) -> loop& = delete;
  loop(loop&&) = delete;
  auto operator=(loop&&) -> loop& = delete;

  // Destroys (without running) anything still queued - mirrors
  // future_state<T>'s own destructor: a loop dropped mid-program simply
  // abandons whatever it hadn't gotten to yet, rather than leaking it.
  ~loop() {
    ready_.drain([this](detail::ready_node& node) { node.destroy(allocator_); });
    for (const auto& entry : pending_timers_) {
      entry.node->destroy(allocator_);
    }
  }

  [[nodiscard]] auto allocator() const noexcept -> allocator_type { return allocator_; }

  // Pushes an already-ready unit of work onto the ready-queue, for the
  // next drain pass inside run()/run_until_idle() - called by
  // est::future_state<T>::complete() (est:future) instead of invoking a
  // continuation inline.
  void enqueue_ready(detail::ready_node& node) noexcept { ready_.enqueue(node); }

  // Registers `node` to fire() once `deadline` passes - the primitive a
  // higher-level timer-driven future builds on (est::sleep_for(),
  // est:promise); :loop itself never names est::future/est::promise (see
  // this file's own top comment on why).
  //
  // reserve() before schedule_at(), not after: this method touches two
  // separate containers (timers_, pending_timers_) with no way to roll
  // back a partial update, so the ordering has to guarantee that once
  // timers_ knows about `deadline`, recording it in pending_timers_ can't
  // fail. reserve() can throw (bad_alloc) - but if it does, schedule_at()
  // was never called, so timers_ and pending_timers_ stay in sync. Once
  // reserve() succeeds, push_back() below is guaranteed not to reallocate
  // and pending_entry is a trivial two-member struct, so it can't itself
  // throw. Without this, a push_back() failure *after* a successful
  // schedule_at() would leave a timer id in timers_ with no matching
  // pending_timers_ entry - fire_ready_timers()'s check() below exists
  // exactly to catch that, but check() compiles away entirely under
  // NDEBUG (est:check), turning the desync into a dereference of
  // pending_timers_.end() instead of a caught precondition violation.
  void schedule_timer(detail::timer_node& node, clock::time_point deadline) {
    pending_timers_.reserve(pending_timers_.size() + 1);
    const auto id = timers_.schedule_at(deadline);
    pending_timers_.push_back(pending_entry{.id = id, .node = &node});
  }

  // Runs until both the ready-queue and the timer queue are empty - for
  // tests/examples that shouldn't block forever.
  void run_until_idle() { run_impl(); }

  // The real service loop: drains ready continuations, sleeps until the
  // next timer deadline, repeats. Currently behaves identically to
  // run_until_idle() - the difference PLAN.md anticipates (blocking
  // indefinitely, kept alive by a live I/O reactor with more external
  // wakeup sources than timers) only becomes real once I/O support lands,
  // explicitly out of scope for this milestone; until then nothing could
  // ever wake a fully idle loop back up anyway, so returning is the only
  // sane behavior for both. stop() lets a caller request an even earlier
  // exit, before the loop would otherwise go idle on its own.
  void run() { run_impl(); }

  // Asks run()/run_until_idle() to return after finishing whichever
  // ready continuation it's currently draining, rather than continuing to
  // the next one or sleeping for the next timer deadline. No effect when
  // the loop isn't currently inside run()/run_until_idle().
  void stop() noexcept { stop_requested_ = true; }

private:
  struct pending_entry {
    timer_queue<allocator_type>::id id;
    detail::timer_node* node;
  };

  // Precondition: not already inside a run()/run_until_idle() call on
  // this same loop. Checked, not silently tolerated: reentering would
  // otherwise reset stop_requested_ back to false out from under the
  // outer, still-in-progress call (e.g. a continuation that calls
  // loop.stop() and then loop.run_until_idle() again before returning) -
  // the outer call's `if (stop_requested_) return;` would then never
  // trip, silently losing the stop() request instead of failing loudly.
  // No coroutine machinery exists yet (M4) to make nested pumping a real,
  // supported use case - same debug-checked-precondition stance this
  // codebase already takes elsewhere (est:check) rather than building
  // real reentrant-stop() bookkeeping nothing currently needs.
  void run_impl() {
    check(!running_, "est::loop::run()/run_until_idle() called reentrantly");
    running_ = true;
    scope_exit const not_running_guard{[this]() noexcept { running_ = false; }};
    stop_requested_ = false;
    for (;;) {
      drain_ready();
      if (stop_requested_) {
        return;
      }
      const auto deadline = timers_.next_deadline();
      if (!deadline) {
        return; // nothing ready, nothing pending - idle, nothing left to do
      }
      platform::instance().sleep_until(*deadline);
      fire_ready_timers();
    }
  }

  void drain_ready() {
    while (auto* node = ready_.dequeue()) {
      run_one(*node);
      if (stop_requested_) {
        return;
      }
    }
  }

  // Returns a guard that destroys `node` via this loop's allocator when
  // it goes out of scope, however that happens - the "always destroy
  // after running/firing" pattern both run_one() and fire_ready_timers()
  // need, factored out so a future change to it only has one place to
  // make. Returned by value as a genuine prvalue (never bound to a named
  // variable and then moved) so this compiles despite scope_exit's
  // deleted move constructor - the same guaranteed-copy-elision pattern
  // est::platform::override_instance() already relies on. Defined ahead
  // of run_one()/fire_ready_timers() below, not just declared: a deduced
  // (auto) return type has to be resolved from the function's own body
  // before any caller earlier in the class can use it.
  template <class Node> [[nodiscard]] auto destroy_guard(Node& node) noexcept {
    return scope_exit([&node, this]() noexcept { node.destroy(allocator_); });
  }

  // Runs one ready continuation, timing it against long_running_threshold
  // (docs/PLAN.md, M3's "long-running-callback detection"). Single-
  // threaded means one slow continuation blocks everything else this
  // loop owns, with nothing to preempt it, so a runaway handler should at
  // least show up as a clear diagnostic instead of "the whole program
  // mysteriously stalled." The threshold value is a starting point, not
  // tuned against any real workload - easy to revisit if it turns out to
  // matter in practice (same stance this codebase already takes on other
  // debug-only/best-effort details, docs/PLAN.md).
  void run_one(detail::ready_node& node) {
    const auto guard = destroy_guard(node);
    const auto start = platform::instance().now();
    node.run();
    const auto elapsed = platform::instance().now() - start;
    if (elapsed > long_running_threshold) {
      // Formatting and printing itself is platform::printdbg()'s job, not
      // this loop's - it already owns the "best-effort, nothrow diagnostic
      // straight to std::cerr" pattern (see its own doc comment).
      platform::printdbg(
          "est::loop: a continuation took {}ms (> {}ms threshold) to run",
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(long_running_threshold).count());
    }
  }

  void fire_ready_timers() {
    const auto now = platform::instance().now();
    while (const auto id = timers_.pop_ready(now)) {
      const auto it = std::ranges::find(pending_timers_, *id, &pending_entry::id);
      check(it != pending_timers_.end(), "loop: fired timer id missing from pending_timers_");
      auto* node = it->node;
      pending_timers_.erase(it);
      const auto guard = destroy_guard(*node);
      node->fire();
    }
  }

  static constexpr clock::duration long_running_threshold = std::chrono::milliseconds(50);

  allocator_type allocator_;
  intrusive_list<detail::ready_node> ready_;
  timer_queue<allocator_type> timers_;
  std::pmr::vector<pending_entry> pending_timers_;
  bool stop_requested_ = false;
  bool running_ = false;
};

} // namespace est
