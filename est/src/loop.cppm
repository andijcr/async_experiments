export module est:loop;

import std;
import :check;
import :platform;
import :timer;
import :util.intrusive_list;
import :util.scope_exit;

export namespace est {

// The (small, fixed) set of levels a ready-to-run continuation
// (est::detail::ready_node's own priority_level field, below) carries -
// which of loop's per-level ready queues a scheduler drains from next.
// Lives here, not alongside current_loop()/current_allocator() in
// :util.current_loop, because :loop cannot import that partition back (it
// already imports :loop) - the same constraint that already keeps
// detail::current_allocator_new_delete<T> a separate mixin there instead
// of living directly on ready_node/timer_node themselves (see ready_node's
// own doc comment below). Four levels, not an open-ended count: issue #31
// settled on "a few levels suffice" rather than a numeric/unbounded scale.
// max_priority aliases the highest real level (not a fifth one) so
// loop::priority_levels (below) can be derived from it instead of
// carrying its own separately-maintained "4".
// NOLINTNEXTLINE(readability-enum-initial-value)
enum class Priority : std::uint8_t { background, normal, high, critical, max_priority = critical };

} // namespace est

namespace est::detail {

// current_priority()/set_priority()'s own backing storage (export
// namespace est, below) - thread_local for the identical reason
// est::detail::tls_context (est:util.current_loop) is: this codebase's
// eventual target is one loop per core, each with its own independent
// notion of "what priority is ambient right now," no cross-core
// synchronization needed.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline thread_local Priority tls_priority = Priority::normal;

} // namespace est::detail

export namespace est {

// The priority a freshly registered continuation/resume node inherits by
// default - read at registration time (future_state<T>::then()/
// then_fast()'s own `Priority prio = current_priority()` default
// argument, est:future) or explicitly, by a coroutine, right before a
// co_await it wants raised (future_awaiter<T>::await_suspend(), est:future).
// loop::run_one() (below) sets this to whatever priority the
// currently-running node itself carries for the duration of its run() -
// so anything that node goes on to register (another then()/co_await)
// inherits the same priority by default, without threading a parameter
// through every call in between. Defaults to Priority::normal on a thread/
// core nothing has ever raised or lowered.
[[nodiscard]] inline auto current_priority() noexcept -> Priority {
  return detail::tls_priority;
}

// Raises or lowers current_priority() for the caller's own scope -
// modeled on est::make_current_loop()/est::platform::override_instance()'s
// own RAII shape (built the same way, on est::scope_exit), but a plain
// save/restore stack rather than a single slot with a checked precondition
// against nesting: unlike "which loop is current," nested priority scopes
// are the expected, common case (a background chain lowering it inside a
// caller that already raised it), not a programming error.
[[nodiscard]] inline auto set_priority(Priority new_priority) noexcept {
  const auto previous = detail::tls_priority;
  detail::tls_priority = new_priority;
  return scope_exit([previous]() noexcept { detail::tls_priority = previous; });
}

} // namespace est

// Type-erased primitives est::loop's ready-queue and pending-timer list
// hold, kept fully independent of est::future/est::promise: :loop needs a
// type-erased "thing to run" for its ready-queue, and est::future's own
// continuation nodes are exactly that - but future_state<T> also needs to
// depend on est::loop to defer onto it, so :loop cannot import :future
// without a circular module dependency. Resolved by keeping :loop the
// lower-level partition: :future's continuation_node<T> inherits
// ready_node directly instead of :loop naming future_state<T>, and
// est::sleep_for()/sleep_until() (the future<void>-returning sugar built
// on schedule_timer() below) live in :promise instead of here.
// Deliberately not exported: not part of this partition's public
// surface, only visible to another partition that imports :loop.
namespace est::detail {

// One entry in est::loop's ready-queue: something already known to be
// ready to run - a fulfilled future_state<T>'s continuation, or a
// resumed coroutine handle (future_resume_node<T>, est:future).
//
// Destroyed on this base pointer/reference - a bare `delete`
// (abandon_ready_node() below) or a local `std::unique_ptr<ready_node>`'s
// own implicit deletion (loop::run_one() below) alike - safe despite
// the virtual destructor being the only thing this base declares,
// because every concrete node type also has its own `operator new`/
// `operator delete` (usually inherited from
// detail::current_allocator_new_delete<T>, est:util.current_loop -
// resolving est::current_allocator(), the same pattern
// detail::coroutine_frame_alloc()/coroutine_frame_dealloc() use for a
// coroutine frame, est:future): a class with a virtual destructor
// always deallocates through the dynamic type's own visible
// deallocation function, with the dynamic type's own correct size,
// never the static (base) one - deleting through a base pointer, however
// that deletion is spelled, is exactly what a virtual destructor exists
// to make safe. This base itself cannot inherit that same mixin, nor
// define an equivalent default directly: either would need
// est::current_allocator() (est:util.current_loop), which itself
// imports :loop, so :loop importing it back would be circular (see this
// file's own top comment on the same constraint for est::future).
// The exception every abandon() override that needs to actually complete
// something (rather than just deallocate) throws/wraps: ready_node::
// abandon()'s own doc comment below lists them - est:future's
// flatten_forwarder<T>/concrete_continuation<Fn, U>, est:promise's
// sleep_resume_node/promise_resume_node<T>. One shared, message-less type
// instead of each call site hand-rolling its own std::runtime_error(a
// slightly different literal) - nothing anywhere in this codebase ever
// inspects what() to tell one abandonment apart from another (the
// completed promise/future's own exception_ptr is only ever observed as
// "this failed," never matched against particular text), so the message
// never carried information a caller could act on in the first place.
class abandoned_exception : public std::runtime_error {
public:
  abandoned_exception() : std::runtime_error("est: abandoned") {}
};

class ready_node : public intrusive_list_node {
public:
  ready_node() = default;
  ready_node(const ready_node&) = delete;
  auto operator=(const ready_node&) -> ready_node& = delete;
  ready_node(ready_node&&) = delete;
  auto operator=(ready_node&&) -> ready_node& = delete;
  virtual ~ready_node() = default;

  virtual void run() = 0;

  // Which of loop's per-level ready queues this node belongs on
  // (est::Priority, above) - set by whatever registers this node
  // (future_state<T>::then_impl()/set_continuation(), est:future) after
  // construction, before it's ever handed to enqueue_ready() below; every
  // concrete node type defaults to Priority::normal by simply never
  // touching this field. A plain public field, not a getter/setter pair -
  // matching intrusive_list_node::next's own already-public, no-accessor
  // style (est:util.intrusive_list): there is no invariant here to
  // protect, just a value loop::enqueue_ready() reads and a caller sets.
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  Priority priority_level = Priority::normal;

  // Called exactly once, immediately before a node that never ran is
  // deleted (loop::drain_pending() below) - never called on a node that
  // did run (loop::run_one() below deletes those directly, no
  // abandon() call). Default is a no-op; a node whose completion needs
  // to differ on this abandoned path - est:future's future_resume_node<T>/
  // concrete_continuation<Fn, U>/flatten_forwarder<T>, est:promise's
  // sleep_resume_node/promise_resume_node<T> (the latter shared with
  // est:sync.event) - overrides it instead of every call site branching
  // on a `ran` flag itself.
  virtual void abandon() noexcept {}
};

// One entry in est::loop's pending-timer list: something to run once a
// deadline passes. Not itself part of any intrusive list (unlike
// ready_node) - est::loop tracks pending timer_nodes in its own
// pending_timers_ vector, keyed by est::timer_queue's own id, since
// nothing needs to walk them in deadline order beyond what timer_queue's
// min-heap already provides.
//
// Destroyed the same way ready_node above is, and for the identical
// reason - see that class's own doc comment.
class timer_node {
public:
  timer_node() = default;
  timer_node(const timer_node&) = delete;
  auto operator=(const timer_node&) -> timer_node& = delete;
  timer_node(timer_node&&) = delete;
  auto operator=(timer_node&&) -> timer_node& = delete;
  virtual ~timer_node() = default;

  virtual void fire() = 0;

  // Same contract as ready_node::abandon()'s own doc comment - called
  // once, immediately before a timer_node that never fired is deleted.
  virtual void abandon() noexcept {}
};

// A registered source of external change - est::external_event<T>'s own
// opt-in loop-registration (est:sync.external_event) is the one thing
// that implements this today. Type-erased the same way ready_node/
// timer_node above are, but deliberately simpler: loop never owns or
// deletes one of these (register_external()/unregister_external() below
// just add/remove a raw observer pointer), since the registrant's own
// lifetime is managed entirely by whoever constructed it - unlike
// ready_node/timer_node, which loop itself allocates/frees via
// est::current_allocator().
class external_source {
public:
  external_source() = default;
  external_source(const external_source&) = delete;
  auto operator=(const external_source&) -> external_source& = delete;
  external_source(external_source&&) = delete;
  auto operator=(external_source&&) -> external_source& = delete;
  virtual ~external_source() = default;

  // Loop-thread only, exactly like est::external_event<T>::poll()'s own
  // existing contract (est:sync.external_event) - called from
  // loop::poll_external_if_pending() below, never from any other
  // context.
  virtual void poll() noexcept = 0;
};

// The "this node never ran/fired - complete it however abandon() does
// that, then free it" pattern every drain-without-running call site
// needs: loop::drain_pending() (below, for both containers it drains),
// and every future_state<T>/counting_event<Mode> destructor that drains
// a still-queued waiters_ list (est:future, est:sync.event - est::mutex,
// est:sync.mutex, has neither any more, having no waiters_ of its own to
// drain) - repeating the two-line body inline at each of those call
// sites would just be the same pair of statements copied several
// times. Two separate, non-overloaded functions rather than one
// overloaded on `ready_node&`/`timer_node&`: `intrusive_list<ready_node>::
// drain(Fn)` (used by every one of the waiters_ call sites above) deduces
// its own `Fn` template parameter directly from the function passed to
// it, which only works for a name that resolves to exactly one type -
// an overloaded name has no single type to deduce until *after*
// overload resolution has already picked one, so passing it straight to
// a deducing `Fn` parameter is ill-formed, not just a matter of which
// overload a reader would expect to be picked.
//
// `owned` takes ownership of `node` before abandon() ever runs, not
// after: constructing a `unique_ptr` doesn't itself call anything on the
// object it holds, so `owned->abandon()` still runs first, with the
// implicit deletion happening only once this function returns - the
// same ordering the old, hand-written `node.abandon(); delete &node;`
// pair had. `unique_ptr<ready_node>`/`unique_ptr<timer_node>`, not the
// concrete node's own type: exactly like a bare `delete` through this
// base reference already relied on (ready_node's own doc comment,
// above), deletion resolves through the dynamic type's own vtable slot
// either way - a virtual destructor is precisely what makes owning (and
// deleting) a polymorphic object through its base type safe, whichever
// of the two spellings does the deleting.
inline void abandon_ready_node(ready_node& node) noexcept {
  const std::unique_ptr<ready_node> owned(&node);
  owned->abandon();
}
inline void abandon_timer_node(timer_node& node) noexcept {
  const std::unique_ptr<timer_node> owned(&node);
  owned->abandon();
}

} // namespace est::detail

export namespace est {

// The single-threaded run loop: owns a ready-queue of already-fulfilled
// continuations and a timer_queue, and is the thing that actually
// resumes continuations once a promise is fulfilled or a timer fires -
// est::future_state<T>::complete() (est:future) defers to it via
// enqueue_ready() instead of invoking a continuation inline on the
// fulfilling call stack.
//
// An explicit object a caller constructs, not a global singleton like
// est::platform::instance(): unlike platform (a stateless vtable swap), a
// loop carries real mutable state (the ready-queue, pending timers) a
// shared global would accumulate cross-test contamination in - every test
// that needs one constructs its own. A caller registers it as
// est::current_loop() via est::make_current_loop() (est:util.current_loop)
// for make_promise_future()/sleep_for()/mutex/counting_event/a coroutine's
// own promise_type to find; nothing here depends on that registration
// directly.
//
// Precondition: a loop must outlive every future_state<T> (and therefore
// every promise<T>/future<T>/then()-chain), est::mutex, or
// est::counting_event that ever resolved this loop via current_loop() -
// none of them hold a loop& of their own; each resolves current_loop()
// fresh at the point of use instead (see their own doc comments for the
// cross-loop hazard that design carries). There is no way to check a
// dangling reference at runtime.
class loop {
public:
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;
  using clock = platform::clock;
  using timer_id = timer_queue<allocator_type>::id;
  using WakeId = platform::interface::WakeId;

  // One FIFO ready-queue per priority level (est::Priority, above) -
  // scheduler_type (below) picks which of these a given drain pass pops
  // from next.
  static constexpr std::size_t priority_levels =
      static_cast<std::size_t>(Priority::max_priority) + 1;
  using ready_queues = std::array<intrusive_list<detail::ready_node>, priority_levels>;

  // The pluggable drain policy: given every level's own queue, pops and
  // returns whichever node should run next (nullptr once every level is
  // empty) - it does the pop itself, not just picks a level, since a
  // fairness-oriented policy (issue #31's "proportionate" scheduler, not
  // implemented yet) needs to track its own per-level state to decide
  // that, not just react to whichever level loop happens to ask about.
  // Type-erased, not a virtual base: unlike est::platform::interface (the
  // framework's one deliberate runtime-polymorphic OOP seam), a scheduler
  // decision happens on the hottest path this codebase has - once per
  // ready node, every single one - so this is meant to be the same kind
  // of type erasure est::shared_ptr's std::pmr::polymorphic_allocator
  // already is throughout this codebase, not a second inheritance
  // hierarchy alongside platform::interface.
  //
  // std::function, not std::move_only_function as issue #31 itself
  // proposed: the pinned Clang 22 libc++ snapshot this codebase builds
  // against doesn't implement std::move_only_function yet
  // (__cpp_lib_move_only_function is undefined) - std::function's own
  // copy-constructible-callable requirement costs nothing in practice for
  // a scheduler (the eager one below is a stateless function pointer; a
  // future fairness-oriented one only needs plain, copyable per-level
  // counters), so this is a pragmatic substitute, not a design change -
  // worth revisiting once libc++ catches up.
  using scheduler_type = std::function<detail::ready_node*(ready_queues&)>;

  // The default scheduler: always drains the highest non-empty level
  // first - simple, and matches "critical actually means critical," at
  // the accepted cost that a busy higher level can starve a lower one
  // indefinitely (issue #31's own open question; a fairness-oriented
  // "proportionate" alternative is a documented follow-up, not built
  // here). Priority's own declaration order (background < normal < high
  // < critical) doubles as the scan order, highest value first.
  [[nodiscard]] static auto eager_scheduler(ready_queues& queues) noexcept -> detail::ready_node* {
    for (auto& level : std::ranges::reverse_view(queues)) {
      if (auto* node = level.dequeue()) {
        return node;
      }
    }
    return nullptr;
  }

  // `id` is meaningless on every backend today (each one only ever drives
  // a single loop per platform::interface instance, so wake()/wake_all()
  // don't need to tell loops apart yet) - it exists now so a future
  // multi-loop target (issue #125: one loop per core, cores waking each
  // other) doesn't need an interface-breaking change later. A caller not
  // planning to be woken across threads/cores never needs to touch it.
  explicit loop(allocator_type allocator = {},
                scheduler_type scheduler = eager_scheduler,
                WakeId id = WakeId{0})
      : allocator_(allocator), scheduler_(std::move(scheduler)), id_(id), timers_(allocator),
        pending_timers_(allocator), external_sources_(allocator) {}
  loop(const loop&) = delete;
  auto operator=(const loop&) -> loop& = delete;
  loop(loop&&) = delete;
  auto operator=(loop&&) -> loop& = delete;

  // Destroys (without running) anything still queued - mirrors
  // future_state<T>'s own destructor: a loop dropped mid-program simply
  // abandons whatever it hadn't gotten to yet, rather than leaking it.
  // Always safe to call directly: drain_pending() below (also called by
  // est::make_current_loop()'s own returned guard, est:util.current_loop)
  // is idempotent, so it doesn't matter whether that already ran.
  ~loop() { drain_pending(); }

  [[nodiscard]] auto allocator() const noexcept -> allocator_type { return allocator_; }

  // Pushes an already-ready unit of work onto the ready-queue, for the
  // next drain pass inside run()/run_until_idle() - called by
  // est::future_state<T>::complete() (est:future) instead of invoking a
  // continuation inline. Routed into the level `node`'s own priority_level
  // (ready_node's own field, above) names - already set by whoever
  // registered `node` (future_state<T>::then_impl()/set_continuation(),
  // est:future), defaulting to Priority::normal for anything that never
  // touches it.
  void enqueue_ready(detail::ready_node& node) noexcept {
    // node.priority_level is Priority, a 4-valued enum (background through
    // critical) - static_cast<size_t> of it is always in [0, priority_levels),
    // so this index is safe by construction despite not being a compile-time
    // constant.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    ready_[static_cast<std::size_t>(node.priority_level)].enqueue(node);
  }

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
  //
  // Returns the id timer_queue::schedule_at() assigned, so a caller that
  // wants to cancel this specific registration later can do so via
  // cancel_timer() below - most callers (sleep_until() without a
  // stop_token, schedule_periodic()'s own re-arming) simply discard it.
  auto schedule_timer(detail::timer_node& node, clock::time_point deadline) -> timer_id {
    pending_timers_.reserve(pending_timers_.size() + 1);
    const auto id = timers_.schedule_at(deadline);
    pending_timers_.push_back(pending_entry{.id = id, .node = &node});
    return id;
  }

  // Cancels a still-pending registration from schedule_timer() above,
  // completing it the same way drain_pending() completes every other
  // still-queued timer_node on teardown - abandon(), then destroy. Returns
  // false (a no-op) if `id` already fired or was already cancelled: unlike
  // drain_pending() (which unconditionally walks every entry), this has to
  // find one specific entry first, since the timer may have already fired
  // and been erased by fire_ready_timers() by the time a caller gets
  // around to cancelling it - a race a token-driven canceller (est:promise's
  // token-aware sleep_for()/sleep_until()) can't rule out ahead of time.
  [[nodiscard]] auto cancel_timer(timer_id id) noexcept -> bool {
    const auto it = std::ranges::find(pending_timers_, id, &pending_entry::id);
    if (it == pending_timers_.end()) {
      return false;
    }
    timers_.cancel(id);
    auto* node = it->node;
    pending_timers_.erase(it);
    detail::abandon_timer_node(*node);
    return true;
  }

  [[nodiscard]] auto id() const noexcept -> WakeId { return id_; }

  // Registers `source` to have its poll() called the next time this loop
  // notices notify_external() was called since the last check
  // (poll_external_if_pending() below) - est::external_event<T>'s own
  // opt-in loop-registration constructor (est:sync.external_event) is
  // the only caller today. Unregister via unregister_external() below
  // before `source` is destroyed; est::external_event<T>'s own
  // destructor does this - the same "loop must outlive me" precondition
  // every other loop-adjacent type already carries, not a new hazard
  // class.
  void register_external(detail::external_source& source) { external_sources_.push_back(&source); }

  // Removes a prior register_external() registration. Tolerates `source`
  // not being found (already removed, or never registered) rather than
  // asserting - a caller unregistering from its own destructor has no
  // cheap way to already know whether registration happened, the same
  // reasoning cancel_timer() above tolerates an already-fired id.
  void unregister_external(detail::external_source& source) noexcept {
    std::erase(external_sources_, &source);
  }

  // Flags "at least one registered external_source may have changed" -
  // called by est::external_notifier (est:sync.external_event), safe
  // from any context that can touch a std::atomic<bool> at all
  // (mainline, another thread, an ISR). Never blocks, never itself walks
  // external_sources_ - poll_external_if_pending() below does that, on
  // the loop thread only, the next time drain_ready() checks.
  void notify_external() noexcept { pending_external_.store(true, std::memory_order_release); }

  // Runs until both the ready-queue and the timer queue are empty - for
  // tests/examples that shouldn't block forever.
  void run_until_idle() { run_impl(); }

  // The real service loop: drains ready continuations, sleeps until the
  // next timer deadline, repeats. Currently behaves identically to
  // run_until_idle() - the difference (blocking indefinitely, kept alive
  // by a live I/O reactor with more external wakeup sources than timers)
  // only becomes real once I/O support exists; until then nothing could
  // ever wake a fully idle loop back up anyway, so returning is the only
  // sane behavior for both. stop() lets a caller request an even earlier
  // exit, before the loop would otherwise go idle on its own.
  void run() { run_impl(); }

  // Asks run()/run_until_idle() to return after finishing whichever
  // ready continuation it's currently draining, rather than continuing to
  // the next one or sleeping for the next timer deadline. No effect when
  // the loop isn't currently inside run()/run_until_idle().
  void stop() noexcept { stop_requested_ = true; }

  // Destroys (without running) every still-queued ready continuation and
  // pending timer - exactly what ~loop() itself does (and still calls
  // this for, unconditionally, so a caller that never touches this
  // method directly sees no change).
  // Exposed as its own method so est::make_current_loop()'s own returned
  // guard (est:util.current_loop) can call it *before* unregistering this
  // loop as est::current_loop(), not only implicitly via ~loop() after:
  // deleting a node here resolves est::current_allocator() fresh, inside
  // that concrete node type's own operator delete (see ready_node/
  // timer_node's own doc comments above), rather than through any
  // allocator cached by this loop - and abandon()ing a still-suspended
  // coroutine's resume node can itself go on to destroy that coroutine's
  // frame, whose operator delete resolves est::current_allocator() the
  // same way (detail::coroutine_frame_dealloc(), est:future). Draining
  // only later, from ~loop(), after the guard has already cleared the
  // current-loop slot, would resolve those lookups against whatever loop
  // (if any) is current *next*, silently deallocating through the wrong
  // loop's allocator instead of this one's.
  //
  // Idempotent: safe to call more than once. A first call already drains
  // both containers to empty, so a later call (from ~loop(), typically)
  // finds nothing left to do.
  //
  // timers_.cancel(entry.id) before destroying each node, not just
  // clearing pending_timers_ on its own: timers_ (the timer_queue min-heap
  // schedule_timer() also records the deadline in) is a *separate*
  // member, and this method can now run on a loop that keeps going
  // afterward (called from make_current_loop()'s guard, not only from
  // ~loop() right before the whole object - timers_ included - goes
  // away). Without the cancel(), a still-live loop's own timers_ would
  // keep a stale deadline for a node that no longer exists in
  // pending_timers_ - fire_ready_timers()'s own check() exists exactly to
  // catch that desync when it later tries to look the id back up.
  //
  // pending_timers_ drained *before* ready_, not the more obvious other
  // way around: a timer node's abandon() can complete its promise with
  // an exception (detail::sleep_resume_node, est:promise), and that
  // completion, like any other, drains the future_state's own waiters
  // onto *this* loop's ready_ via enqueue_ready(). Draining
  // pending_timers_ first means anything it cascades into ready_ lands
  // there before ready_.drain() runs - and drain() re-checks after every
  // node it destroys (`intrusive_list<T>::drain()`'s own while-loop), so
  // it picks up anything appended during its own pass. The reverse order
  // would leave a cascade appended after ready_.drain() has already
  // finished, leaking whatever it was keeping alive (a coroutine's own
  // frame, concretely). Nothing in this codebase's node types ever
  // cascades the other direction (ready_ back into pending_timers_), so
  // a single pass in this order is sufficient.
  void drain_pending() noexcept {
    for (const auto& entry : pending_timers_) {
      timers_.cancel(entry.id);
      detail::abandon_timer_node(*entry.node);
    }
    pending_timers_.clear();
    for (auto& level : ready_) {
      level.drain(detail::abandon_ready_node);
    }
  }

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
  // Nested pumping isn't a supported use case; this is a debug-checked
  // precondition (est:check) rather than real reentrant-stop()
  // bookkeeping.
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
      platform::instance().interruptible_sleep_until(*deadline);
      fire_ready_timers();
    }
  }

  // poll_external_if_pending() is checked at the top of every iteration,
  // not as a separate step after interruptible_sleep_until() above - this
  // one call site already covers both: drain_ready() is unconditionally
  // re-entered as the very first thing run_impl()'s own loop does once
  // interruptible_sleep_until()/fire_ready_timers() return, so an early
  // wake is picked up here without a second, separate check.
  void drain_ready() {
    for (;;) {
      poll_external_if_pending();
      auto* node = scheduler_(ready_);
      if (node == nullptr) {
        return;
      }
      run_one(*node);
      if (stop_requested_) {
        return;
      }
    }
  }

  // One atomic exchange, checked whether or not anything is registered -
  // effectively free in the common case nothing external is happening
  // (issue #125's own "cost when unused" question). Only pays for the
  // O(external_sources_.size()) walk once the flag was actually observed
  // set. Plain range-for, not index-based: poll()'s own contract only
  // ever calls binary_event::set() (est:sync.event), which itself only
  // ever calls loop::enqueue_ready() - never runs a woken continuation
  // inline - so nothing a poll() call does can reentrantly mutate
  // external_sources_ during this walk.
  void poll_external_if_pending() noexcept {
    if (!pending_external_.exchange(false, std::memory_order_acquire)) {
      return;
    }
    for (auto* source : external_sources_) {
      source->poll();
    }
  }

  // Runs one ready continuation, checking it against
  // long_running_threshold. Single-threaded means one slow continuation
  // blocks everything else this loop owns, with nothing to preempt it,
  // so a runaway handler should at least show up as a clear diagnostic
  // instead of "the whole program mysteriously stalled." The threshold
  // value is a starting point, not tuned against any real workload.
  //
  // The actual timing/measuring/printing is platform::interface's job,
  // not this loop's (reset_loop_stall_detection()/detect_loop_stall(),
  // platform.cppm) - this loop only calls the two bracketing hooks and
  // owns the threshold value itself, same division of responsibility as
  // sleep_until() already has: est::loop decides *what* it needs, the
  // installed platform decides *how* to provide it. A future backend
  // could watch for a stall on its own background thread instead of only
  // checking synchronously here, without this call site changing at all.
  // `guard` exists purely for its destructor's side effect - `node`
  // itself is what run() is actually called on below, never `guard` -
  // guaranteeing the node is deleted no matter how this function
  // returns, even though run() itself already catches every exception a
  // callback could throw internally (this is a defensive guarantee, not
  // something the happy path relies on). Deletion through
  // `unique_ptr<detail::ready_node>` (the base type, not whatever
  // concrete node this actually is) resolves through the dynamic type's
  // own vtable slot, exactly like a bare `delete` through the same
  // reference would - a virtual destructor makes that safe regardless of
  // which spelling triggers it. Used inline here and in
  // fire_ready_timers() below rather than through a shared helper: once
  // this shrank to exactly `unique_ptr<Node>(&node)`, a template wrapping
  // it stopped earning its keep over writing the same one line twice.
  //
  // static: touches no member of *this any more - long_running_threshold
  // (below) is itself a static member, and everything else here is
  // either the node parameter, est::platform::instance(), or the ambient
  // current_priority()/set_priority() thread_local (est::Priority, above)
  // - now that destroy_guard() (a non-static member, needing an implicit
  // `this` to call at all) is gone from this body, nothing left requires
  // an instance. drain_ready() below still calls this the same way either
  // way (`run_one(*node)`, implicitly `this->run_one(*node)` for a
  // static member too).
  //
  // priority_guard set before the stall-detection bracket, not after:
  // this is what makes "the main chain of computation inherits its
  // priority" (issue #31) actually happen - anything `node.run()` itself
  // goes on to register (another then()/co_await) reads current_priority()
  // as its own default, so it needs to already read back `node`'s own
  // priority_level for the whole duration of this call, not just around
  // it.
  static void run_one(detail::ready_node& node) {
    const std::unique_ptr<detail::ready_node> guard(&node);
    const auto priority_guard = set_priority(node.priority_level);
    platform::instance().reset_loop_stall_detection();
    node.run();
    platform::instance().detect_loop_stall(long_running_threshold);
  }

  void fire_ready_timers() {
    const auto now = platform::instance().uptime();
    while (const auto id = timers_.pop_ready(now)) {
      const auto it = std::ranges::find(pending_timers_, *id, &pending_entry::id);
      check(it != pending_timers_.end(), "loop: fired timer id missing from pending_timers_");
      auto* node = it->node;
      pending_timers_.erase(it);
      const std::unique_ptr<detail::timer_node> guard(node);
      node->fire();
    }
  }

  static constexpr clock::duration long_running_threshold = std::chrono::milliseconds(50);

  allocator_type allocator_;
  scheduler_type scheduler_;
  WakeId id_;
  ready_queues ready_;
  timer_queue<allocator_type> timers_;
  std::pmr::vector<pending_entry> pending_timers_;
  bool stop_requested_ = false;
  bool running_ = false;
  std::atomic<bool> pending_external_{false};
  std::pmr::vector<detail::external_source*> external_sources_;
};

} // namespace est
