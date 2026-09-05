export module est:loop;

import std;
import :check;
import :platform;
import :sync.mutex;
import :timer;
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
class ready_node : public mutex_waiter {
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
    while (auto* waiter = ready_.dequeue()) {
      // Safe by construction, not by RTTI: every waiter ever enqueued
      // into ready_ is a detail::ready_node (enqueue_ready() only
      // accepts one) - there is no dynamic_cast alternative worth paying
      // for here, same reasoning est:future's own former waiter_node
      // downcast used.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
      static_cast<detail::ready_node*>(waiter)->destroy(allocator_);
    }
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
  void schedule_timer(detail::timer_node& node, clock::time_point deadline) {
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

  void run_impl() {
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
    while (auto* waiter = ready_.dequeue()) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
      run_one(*static_cast<detail::ready_node*>(waiter));
      if (stop_requested_) {
        return;
      }
    }
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
    scope_exit const guard{[&node, this]() noexcept { node.destroy(allocator_); }};
    const auto start = platform::instance().now();
    node.run();
    const auto elapsed = platform::instance().now() - start;
    if (elapsed > long_running_threshold) {
      // Best-effort diagnostic only, same stance as
      // platform::hosted_linux::assert_failure()'s own std::println call
      // - a failure to print this warning isn't itself worth stopping
      // the loop over.
      try {
        std::println(
            std::cerr,
            "est::loop: a continuation took {}ms (> {}ms threshold) to run",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            std::chrono::duration_cast<std::chrono::milliseconds>(long_running_threshold).count());
        // NOLINTNEXTLINE(bugprone-empty-catch)
      } catch (...) {
      }
    }
  }

  void fire_ready_timers() {
    const auto now = platform::instance().now();
    while (const auto id = timers_.pop_ready(now)) {
      const auto it = std::ranges::find(pending_timers_, *id, &pending_entry::id);
      check(it != pending_timers_.end(), "loop: fired timer id missing from pending_timers_");
      auto* node = it->node;
      pending_timers_.erase(it);
      scope_exit const guard{[node, this]() noexcept { node->destroy(allocator_); }};
      node->fire();
    }
  }

  static constexpr clock::duration long_running_threshold = std::chrono::milliseconds(50);

  allocator_type allocator_;
  waiter_list ready_;
  timer_queue<allocator_type> timers_;
  std::pmr::vector<pending_entry> pending_timers_;
  bool stop_requested_ = false;
};

} // namespace est
