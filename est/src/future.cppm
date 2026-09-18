export module est:future;

import std;
import :check;
import :loop;
import :util.current_loop;
import :util.intrusive_list;
import :util.shared_ptr;

// future_state is intentionally *not* exported: it's the single owned
// object behind an est::promise<T>/est::future<T> pair, not part of the
// public API those two handles present (see its own doc comment below).
// Still visible to other partitions of this module that import :future
// (est:promise does, to hold a shared_ptr<future_state<T>>) - module
// visibility across partitions of the same module doesn't require
// export, only visibility to code outside the module does.
namespace est {
template <class T> class future_state;
}

export namespace est {
template <class T> class future;
}

namespace est::detail {

// The T-dependent half of a queued continuation, sitting on top of
// est:loop's own type-erased detail::ready_node (see that partition's
// doc comment on why :loop can't instead depend on :future): adds only
// the one thing that actually needs T (owner_, and bind_owner() to set
// it). Not nested inside future_state<T> - it's an implementation detail
// of :future, not part of future_state's public surface, so it lives
// here instead.
//
// Deliberately *not* the home of run() itself: an earlier version of
// this class implemented ready_node::run() once, here, as
// `invoke(*owner_)`, with each concrete node instead overriding a
// second, separately virtual invoke(future_state<T>&). That bought
// nothing - the only thing invoke() was ever called with was *owner_,
// so state was always just this class's own owner_ read back through a
// parameter - while costing a second, non-devirtualizable virtual call
// (est::loop's ready_.dequeue() only ever holds a ready_node&, so
// run()'s own dispatch can't statically know which invoke() override
// it's about to make a *second* indirect call to) on every single
// continuation this framework ever runs. bind_owner()/owner_ stays
// shared here since sharing it is genuinely free - it's a plain data
// member and a non-virtual setter, not something a second vtable slot
// was ever paying for - but run() itself now belongs to each concrete
// node below (concrete_continuation<Fn, U>, flatten_forwarder<T>,
// future_resume_node<T>), which reads owner_ directly instead of
// through a parameter. See issue #63 / docs/PLAN.md for the reasoning.
//
// Written *before* future_state<T> (below) in this file, but owner_'s
// shared_ptr<future_state<T>> only actually needs future_state<T>
// complete once this template is first instantiated for a real T - which
// never happens until concrete_continuation/future_resume_node<T>
// (both further down this file, after future_state<T>'s own definition)
// actually use it. See future_state<T>::waiters_'s own doc comment for
// why that laziness matters: keying waiters_ on continuation_node<T>
// directly instead would force eager instantiation, and with it a
// circular completeness requirement between the two classes.
template <class T> class continuation_node : public detail::ready_node {
public:
  // Called exactly once, by future_state<T>::complete()/
  // set_continuation() right before this node is handed to
  // est::loop::enqueue_ready() - takes real shared ownership of the
  // future_state so it survives the gap between being enqueued and the
  // loop actually draining it. Deliberately *not* done at construction
  // time: a node still only pending in future_state<T>'s own waiters_
  // (not yet ready) is already exclusively owned (by pointer, not
  // shared_ptr) by that same future_state - binding a permanent
  // shared_ptr back to it from there on would be a self-reference cycle
  // (the future_state would count itself as one of its own owners),
  // keeping an abandoned, never-completed future_state alive forever
  // instead of destroying it (and its still-pending nodes) normally.
  void bind_owner(shared_ptr<future_state<T>> owner) { owner_ = std::move(owner); }

protected:
  // Protected, not private: every concrete node below reads this
  // directly from its own run() override instead of through a passed-in
  // parameter (see this class's own doc comment above for why).
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  shared_ptr<future_state<T>> owner_;
};

// The node behind then()'s monadic-flatten path
// (future_state<T>::concrete_continuation<Fn, U>::fulfill(), below): once
// registered directly on an inner future_state<T> (via set_continuation(),
// bypassing future<T>/then() entirely), forwards that inner future's
// result - or exception - into a downstream future_state<T> with no
// callback, no closure, and no wrapped/unwrapped dispatch to pick between:
// the inner future's exact value type T is already known and fixed by the
// call site, so there's nothing left to genericize over. Templated on T
// alone - every flatten at the same T reuses this one instantiation
// instead of minting a fresh one per (T, Fn, U) call site.
template <class T>
class flatten_forwarder final : public continuation_node<T>,
                                public current_allocator_new_delete<flatten_forwarder<T>> {
public:
  explicit flatten_forwarder(shared_ptr<future_state<T>> downstream)
      : downstream_(std::move(downstream)) {}

  // Reads owner_ (continuation_node<T>) directly rather than taking a
  // future_state<T>& parameter - see continuation_node<T>'s own doc
  // comment on why run() lives here now instead of behind a second,
  // separately virtual invoke().
  //
  // The state.ready_with_failure() branch lives inside the same try as the success
  // path, not before it (an earlier version of this method had it as an
  // early return ahead of the try, unlike concrete_continuation<Fn, U>'s
  // own identical branch, which was always inside its try) - nothing on
  // this path currently throws (get_exception() is noexcept, and
  // set_exception() itself doesn't throw short of bad_alloc or a
  // check()-triggered assert_failure(), which is noexcept and terminates
  // rather than unwinds), but there's no reason for this method to be
  // the one place in this codebase that assumes so structurally, when
  // catching it costs nothing.
  void run() final {
    auto& state = *this->owner_;
    // __cpp_exceptions (the standard SD-6 feature-test macro - defined
    // only when exceptions are enabled), not a project-specific gate:
    // nothing on this path can actually throw once exceptions are off
    // (-fno-exceptions rejects `throw`/`try`/`catch` everywhere in the
    // TU, not just here - libc++'s own internal "exceptions" route
    // through a termination handler instead), so the try/catch is
    // genuinely unreachable dead code on such a build, not a behavior
    // this class gives up - it just can't be spelled the same way.
    // Needed for the wasm32 backend (docs/PLAN.md's "Issue #99" entry):
    // its compiler crashes on this class when the try/catch is compiled
    // under real wasm exception-handling support instead.
#ifdef __cpp_exceptions
    try {
#endif
      if (state.ready_with_failure()) {
        downstream_->set_exception(state.get_exception());
      } else if constexpr (std::is_void_v<T>) {
        downstream_->set_value();
      } else {
        downstream_->set_value(std::move(state).get());
      }
#ifdef __cpp_exceptions
    } catch (...) {
      downstream_->set_exception(std::current_exception());
    }
#endif
  }

  // Called only when run() never happened: the inner future_state<T>
  // this node was registered on (via set_continuation(), bypassing
  // future<T>/then() entirely - see this class's own top comment) was
  // itself abandoned before ever completing. downstream_ must still be
  // completed here, not silently dropped - see concrete_continuation<Fn,
  // U>'s own doc comment (further down this file) for the full reasoning:
  // a coroutine co_await-ing the outer future<T> this flatten forwards
  // into holds downstream_'s own future_state alive across its own
  // suspension, so dropping just this node's reference to it - without
  // completing it - would strand that coroutine's frame with nothing left
  // to free it. abandoned_exception (est:loop, this same namespace) -
  // shared with every other abandon() override in this codebase that
  // needs to actually complete something, rather than one hand-rolled
  // literal per call site.
  void abandon() noexcept override {
    downstream_->set_exception(std::make_exception_ptr(abandoned_exception()));
  }

  // operator new/delete inherited from current_allocator_new_delete<T>
  // (est:util.current_loop) - see that class's own doc comment for why
  // every concrete ready_node/timer_node needs its own pair rather than
  // one shared at the ready_node/timer_node base itself.

private:
  shared_ptr<future_state<T>> downstream_;
};

// Forward declaration only: future_awaiter<T> (the type future<T>::operator
// co_await() below returns) needs future<T> itself complete - to call
// get() on it in await_resume() - so its own definition has to come after
// future<T>'s, further down this file. This forward declaration is enough
// for future<T> to name it in a friend declaration.
template <class T> class future_awaiter;

// Coroutine frame allocation via a std::pmr::polymorphic_allocator<std::byte>
// - the same allocator convention every other allocation in this codebase
// uses (est::shared_ptr, est::loop's continuation/timer nodes), applied to
// the one allocation this codebase doesn't otherwise control: the
// compiler-generated coroutine frame itself, for a coroutine returning
// est::future<T> (future<T>::promise_type's operator new/delete, below).
// Not templated on T - neither the frame's size nor the allocator depend
// on it, so one shared implementation covers every T.
//
// operator delete only ever gets the frame's size back, never the
// allocator that built it. Rather than stash a memory_resource* alongside
// the frame for operator delete to read back, coroutine_frame_dealloc()
// below simply resolves est::current_allocator() fresh - the same
// "no cached state, look it up at the point of use" design this whole
// experiment applies elsewhere (future_state<T>, est::mutex,
// est::counting_event). This carries the same cross-loop hazard those
// carry, sharpened here: std::pmr::memory_resource::deallocate() requires
// the *same resource instance* that performed the allocation, not merely
// an equivalent one, so a coroutine allocated while one loop was current
// and destroyed after a *different* loop became current is undefined
// behavior, not just misrouted bookkeeping. Callers must not let a
// coroutine outlive the current-loop registration it was created under.
[[nodiscard]] inline auto
coroutine_frame_alloc(std::size_t size, std::pmr::polymorphic_allocator<std::byte> allocator)
    -> void* {
  return allocator.resource()->allocate(size, alignof(std::max_align_t));
}

inline void coroutine_frame_dealloc(void* frame, std::size_t size) noexcept {
  current_allocator().resource()->deallocate(frame, size, alignof(std::max_align_t));
}

// A placeholder "success" alternative for future_state<void>'s result_
// variant: void itself can't be a variant alternative, and reusing
// std::monostate for both "not yet ready" and "ready with no value" would
// make the two states indistinguishable. Never observed by a caller -
// future_state<void>::get() (see below) always returns void, not this
// type.
struct void_value {};

// Pattern-matches Fn's raw then() result against est::future<U> so
// then() can flatten a continuation returning a future into a plain
// future<U> instead of a future<future<U>> - futures are monadic.
// Forward-declared only
// (est::future's own definition comes later in this file); a partial
// specialization only needs to match the template name, not a complete
// type.
template <class> struct is_future : std::false_type {};
template <class U> struct is_future<future<U>> : std::true_type {};
template <class R> inline constexpr bool is_future_v = is_future<R>::value;

template <class R> struct unwrap_future {
  using type = R;
};
template <class U> struct unwrap_future<future<U>> {
  using type = U;
};
template <class R> using unwrap_future_t = unwrap_future<R>::type;

// The "unwrapped" half of then()'s two calling conventions (see
// future_state<T>::then()'s own doc comment): Fn invocable with the
// value itself, or with no arguments at all when T is void. A plain
// `std::invocable<Fn&, const T&>` can't be written unconditionally for
// both cases - forming "const T&" is a hard, non-SFINAE-eligible error
// for T=void (unlike a deduction failure, a directly-named reference to
// void is ill-formed the moment the compiler tries to form the type at
// all, concept or not) - so the two cases are kept in genuinely separate
// (not just short-circuited) if constexpr branches, the same technique
// future_state<T>::get() and then() themselves already rely on.
template <class Fn, class T> consteval auto invocable_unwrapped() -> bool {
  if constexpr (std::is_void_v<T>) {
    return std::invocable<Fn&>;
  } else {
    return std::invocable<Fn&, const T&>;
  }
}

// Constrains then()'s Fn at the template-parameter level, per the two
// calling conventions future_state<T>::then() documents, so an
// incompatible callback fails right at the then() call site with a
// "constraints not satisfied" diagnostic naming this concept, instead of
// failing deep inside then()'s own implementation. future<T>, not
// future_state<T>&, for the wrapped shape - future_state is a detail of
// future (see its own doc comment), not part of the interface a
// then() callback should see.
//
// invocable_unwrapped<Fn, T>() checked first, matching then()'s own
// dispatch order, and not just as a matter of style: constraint
// disjunction (||) short-circuits left-to-right, so whichever operand
// comes first is the one actually evaluated for an Fn that satisfies
// it. A generic callback (an `auto&` lambda, say) that's
// only valid when called with a plain T - e.g. one that returns a copy of
// its argument, which future<T>'s deleted copy constructor makes
// ill-formed for a future<T>& argument - hard-errors (not a graceful
// SFINAE failure - "use of a deleted function" isn't overload-resolution
// failure) if std::invocable<Fn&, future<T>&> is instantiated for it at
// all, checked first or not. Checking invocable_unwrapped first means
// such an Fn is fully satisfied - and short-circuits away from ever
// instantiating the wrapped check - before that ever happens.
template <class Fn, class T>
concept then_callback_for = invocable_unwrapped<Fn, T>() || std::invocable<Fn&, future<T>&>;

} // namespace est::detail

namespace est {

// The single owned object behind an est::promise<T>/est::future<T> pair
// - promise<T>/future<T> are views over this shared state, not the state
// itself. Not exported (see the forward declaration above): a caller
// never sees
// this type directly, only through the est::promise<T>/est::future<T>
// handles that wrap it - including a then() callback registered with
// the "wrapped" calling convention, which receives a real est::future<T>
// (built via shared_from_this(), below) rather than a future_state<T>&.
// Holds value-or-exception storage and a continuation slot built on
// est::intrusive_list. Lifetime is managed externally by an
// est::shared_ptr<future_state<T>> (see make_promise_future() in
// est:promise) - never constructed directly by a caller.
//
// Inherits est::ref_counted (est:util.shared_ptr) rather than the more
// general est::enable_shared_from_this<T>: shared_ptr<future_state<T>>'s
// intrusive specialization allocates this object directly (no separate
// control_block wrapping it), storing the ref count and allocator right
// here instead - see ref_counted's own doc comment for why that also
// makes shared_from_this() (used just below) essentially free, with no
// back-pointer to wire up at construction time. `final` for the same
// reason shared_ptr<T>'s intrusive specialization static_asserts it of
// every T that inherits ref_counted: ref_counted's own destructor is
// deliberately non-virtual (it's only ever destroyed through T*, never
// through ref_counted* - see its own doc comment), so a further-derived
// class here would be destroyed through the wrong type's destructor the
// moment its own ref count reached zero.
//
// Holds no est::loop reference of its own: every method below that needs
// one (the destructor, set_continuation(), then(), complete()) resolves
// est::current_loop() (est:util.current_loop) fresh, at the point of use,
// instead of caching a loop& member set once at construction.
//
// KNOWN HAZARD: because the loop is looked up fresh each time rather than
// fixed at construction, a future_state created while one loop is current
// can end up enqueuing to, or deallocating waiters via, a *different*
// loop if the current-loop registration changes between two calls that
// touch the same future_state (e.g. a continuation registered under loop
// A completing after loop B has become current). A cached loop& member
// made this structurally impossible; resolving fresh does not. Callers
// are responsible for not interleaving distinct est::loop registrations
// across the lifetime of a single future_state/promise/future chain.
template <class T> class future_state final : public ref_counted {
public:
  using continuation_node = detail::continuation_node<T>;

  // What result_ actually stores on success: T itself, except when T is
  // void (variant can't hold void as an alternative), in which case a
  // stand-in tag type is used instead - see detail::void_value.
  using stored_t = std::conditional_t<std::is_void_v<T>, detail::void_value, T>;

  // `allocator`: forwarded straight to ref_counted's own constructor, not
  // used for anything else here - this class already gets its own
  // allocator on demand via current_allocator(), called fresh wherever
  // it's actually needed (then(), the destructor, ...) rather than
  // stored or exposed as a method of its own.
  // shared_ptr<future_state>::make()'s intrusive specialization
  // (this class inherits est::ref_counted, est:util.shared_ptr) always
  // passes it as this constructor's first argument automatically; a
  // caller of make_promise_future() never spells it out.
  explicit future_state(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept
      : ref_counted(allocator) {}

  future_state(const future_state&) = delete;
  auto operator=(const future_state&) -> future_state& = delete;
  future_state(future_state&&) = delete;
  auto operator=(future_state&&) -> future_state& = delete;

  // Destroys (without invoking) any continuation still queued: the
  // promise was dropped without ever completing (a continuation that
  // did become ready was already handed off to est::loop by complete()/
  // set_continuation(), and is that loop's responsibility to destroy,
  // not this future_state's - see loop.cppm). Without this, those
  // still-pending nodes are simply unreachable once this future_state
  // itself is gone - a permanent leak, not just a skipped notification.
  // waiters_.empty() checked first, not just to skip a no-op drain: a
  // future_state that completed with no continuation ever registered (or
  // all of them already drained) can legitimately be destroyed long
  // after whatever loop was current when it was created has stopped
  // being current at all - deleting an abandoned waiter node resolves
  // est::current_allocator() fresh, inside that node's own operator
  // delete (ready_node's own doc comment, est:loop), which would fail
  // its own precondition in that case even though there is nothing here
  // that actually needs one. current_loop() itself is never resolved
  // here at all.
  ~future_state() {
    if (waiters_.empty()) {
      return;
    }
    waiters_.drain(detail::abandon_ready_node);
  }

  void set_value()
    requires std::is_void_v<T>
  {
    check_not_completed();
    result_.template emplace<stored_t>();
    complete();
  }

  // stored_t, not T, in these two signatures - a requires-clause only
  // gates overload resolution, it doesn't stop const T&/T&& from being
  // elaborated (and, for T=void, rejected as "reference to void") the
  // moment future_state<void> itself is instantiated. stored_t is never
  // actually void, so it sidesteps the problem entirely; it's also
  // simply T whenever T isn't void, so this changes nothing observable
  // for every T this class was already used with.
  void set_value(const stored_t& value)
    requires(!std::is_void_v<T>)
  {
    check_not_completed();
    result_.template emplace<stored_t>(value);
    complete();
  }

  void set_value(stored_t&& value)
    requires(!std::is_void_v<T>)
  {
    check_not_completed();
    result_.template emplace<stored_t>(std::move(value));
    complete();
  }

  void set_exception(std::exception_ptr&& exception) {
    check_not_completed();
    result_.template emplace<std::exception_ptr>(std::move(exception));
    complete();
  }

  // Registers a continuation node (already allocated, via its own
  // operator new) to run once ready. If not yet ready, just enqueues it
  // into waiters_ for complete() to drain later - unaffected by
  // run_inline_if_ready below, since there's nothing to run yet on this
  // path.
  //
  // If already ready, the default (run_inline_if_ready = false) hands
  // the node straight to est::loop's ready-queue instead of invoking it
  // here - this future_state never invokes a continuation itself in that
  // case; est::loop always does, on its own drain pass, never inline on
  // this call stack. That default is the same "never invoke a
  // continuation synchronously from within another's own call stack"
  // invariant this codebase applies everywhere else (mutex::unlock(),
  // counting_event<Mode>::set(), ...), so a chain of then() calls of any
  // length always resolves through est::loop's own iterative drain,
  // never by direct recursion on the call stack.
  //
  // run_inline_if_ready = true (then_fast(), below) opts a single node
  // out of that: run() executes right here instead, then the node is
  // deleted immediately - the same "run, then delete" idiom est::loop's
  // own run_one() uses on its own drain pass (est:loop), just performed
  // here directly (via a local unique_ptr<continuation_node> rather than
  // a bare `delete`, purely a style choice - a virtual destructor
  // already makes deleting through this base reference safe either way,
  // see ready_node's own doc comment, est:loop) since run_one() itself
  // is private to est::loop. A caller reaching for this accepts the same
  // recursion-depth responsibility a coroutine's already-ready co_await
  // always had - see then_fast()'s own doc comment. Issue #65.
  void set_continuation(continuation_node& node, bool run_inline_if_ready = false) {
    if (ready()) {
      node.bind_owner(this->shared_from_this());
      if (run_inline_if_ready) {
        const std::unique_ptr<continuation_node> owned(&node);
        // Mirrors loop::run_one()'s own ambient-priority bracket
        // (est:loop) for the deferred path - anything owned->run() itself
        // goes on to register should inherit *this* node's priority too,
        // not whatever was ambient before then_fast() was called.
        const auto priority_guard = set_priority(node.priority_level);
        owned->run();
        return;
      }
      current_loop().enqueue_ready(node);
      return;
    }
    waiters_.enqueue(node);
  }

  [[nodiscard]] auto ready() const noexcept -> bool {
    return !std::holds_alternative<std::monostate>(result_);
  }

  // True once ready() and the stored result is an exception rather than
  // a value - lets a then() continuation that took future_state<T>& (see
  // then() below) inspect success/failure without calling get() (which
  // would rethrow) just to find out.
  [[nodiscard]] auto ready_with_failure() const noexcept -> bool {
    return std::holds_alternative<std::exception_ptr>(result_);
  }

  // The other half of ready_with_failure() above - true once ready() and
  // the stored result is a value rather than an exception. Not just
  // `ready() && !ready_with_failure()` spelled out at each call site:
  // the combination shows up wherever a caller inspects a future after a
  // when_any()-style race to find out which one actually won *and* won
  // cleanly (est::when_any()'s own doc comment, est:when_any) - one call
  // instead of two, with no new state (still read straight off result_,
  // exactly like ready()/ready_with_failure() themselves).
  [[nodiscard]] auto ready_with_value() const noexcept -> bool {
    return ready() && !ready_with_failure();
  }

  // Returns the stored exception_ptr directly, without going through
  // get()'s throw/rethrow. Precondition: ready_with_failure(). Pairs
  // with ready_with_failure() for a caller that already knows there's
  // an exception waiting and wants to forward it without paying for a
  // throw/catch round-trip just to retrieve a pointer - used internally
  // by then()'s
  // unwrapped-mode auto-propagate path and the monadic-flatten
  // forwarding continuation (both below). std::get<...> would throw
  // std::bad_variant_access if the precondition were violated, which -
  // escaping this noexcept function - terminates instead of continuing
  // on bad state; intended fail-fast behavior for a violated
  // precondition, not something to route around.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] auto get_exception() const noexcept -> std::exception_ptr {
    return std::get<std::exception_ptr>(result_);
  }

  // Retrieves the value, or rethrows the stored exception; returns void
  // for future_state<void> (nothing to retrieve, only the rethrow can
  // happen). Precondition: ready(). Deducing this, forwarded straight
  // into std::get<T> rather than branched by hand: called on an lvalue,
  // this returns T& (or const T& for a const lvalue) - non-consuming,
  // safe for the multiple registered then() continuations that each
  // read it without consuming it. Called on an rvalue
  // (std::move(state).get()), this returns T&&, an explicit opt-in to
  // move from the stored value - same caveat as
  // std::optional<T>::value() &&: moving from a value something else
  // (another queued continuation, or a later then()) still needs is the
  // caller's mistake to avoid, not something this class defends
  // against.
  //
  // A mutable (non-const) lvalue call returning T&, not a forced
  // const T&, is a deliberate simplification, not an oversight:
  // future_state is module-private (see this class's own doc comment) -
  // no code outside :future/:promise can even name it, let alone obtain
  // a mutable reference to one and try to mutate result_ through get().
  // Every internal caller (then()'s unwrapped dispatch, future<T>::get()
  // itself) only ever reads the result once and copies or forwards it
  // immediately, so there's nothing left to protect against by forcing
  // const here specifically - unlike, say, a public accessor on a type
  // multiple unrelated callers can reach.
  //
  // No single trailing return type expresses all three cases (void, T&
  // or const T&, T&&) without ever naming the ill-formed "const void&"/
  // "void&&" for T=void, even inside an untaken branch of
  // std::conditional_t (which - unlike if constexpr - instantiates both
  // of its type arguments unconditionally) - so the return type is
  // deduced, and the void case is spelled out under its own if constexpr
  // instead. decltype(auto), not plain auto: plain auto deduction strips
  // references from the return expression's type (the same rule as
  // `auto x = expr;`), which would silently turn the intended
  // non-consuming reference into a full copy of T on every call -
  // decltype(auto) instead takes the return expression's exact type,
  // reference and all, letting std::get<T>'s own overload set (on
  // variant&/const variant&/variant&&) pick the right category straight
  // from std::forward<Self>(self)'s value category - no manual branching
  // needed on top of it.
  //
  // check(self.ready()) above is this function's only *documented*
  // guard, and - like every other est::check() call - compiles away
  // entirely under NDEBUG, at which point a caller violating the
  // precondition is undefined behavior by design (see
  // check_not_completed()'s own comment on this class's general
  // validate-at-boundaries philosophy). std::get<T>() below also throws
  // std::bad_variant_access unconditionally, regardless of NDEBUG, as an
  // accidental (not intentionally designed) second line of defense for
  // T != void - but the T = void branch has nothing left to call
  // std::get on for the same purpose: once ready() holds and the
  // exception alternative has already been ruled out above, result_ can
  // only be holding stored_t (the variant has exactly three
  // alternatives - monostate, stored_t, exception_ptr), so a
  // std::get<stored_t> purely to reconfirm that would be a genuine
  // no-op, not even an accidental check of anything std::get<T> above
  // doesn't already cover for T != void by actually returning a value.
  template <class Self> [[nodiscard]] decltype(auto) get(this Self&& self) {
    check(self.ready());
    if (auto* exception = std::get_if<std::exception_ptr>(&self.result_)) {
      std::rethrow_exception(*exception);
    }
    if constexpr (std::is_void_v<T>) {
      return;
    } else {
      return std::get<T>(std::forward<Self>(self).result_);
    }
  }

  // Registers fn to run once ready. Two calling conventions, chosen by
  // how fn can be invoked:
  //   - fn(const T&), or fn() when T is void: "unwrapped" - called only
  //     on success, with the value itself (or no argument at all for
  //     void). On failure fn is *not* called; the returned future fails
  //     with the same exception instead. (Implemented by simply calling
  //     get() as fn's argument expression: get() rethrows on failure,
  //     which lands in the try/catch below exactly like an exception fn
  //     itself throws.)
  //   - fn(future<T>&): "wrapped" - always called, whether this
  //     future_state succeeded or failed, with a fresh future<T> view of
  //     *this (built via shared_from_this() - future_state is a detail,
  //     not what a callback should see, see this class's own doc
  //     comment); fn inspects ready()/ready_with_failure()/get() to decide what to
  //     do. No implicit unwrap.
  // Checked in that order - unwrapped first - so a generic callback (e.g.
  // an `auto&`/`auto&&` lambda, incidentally invocable both ways) is
  // interpreted as unwrapped by default. Wrapped is only chosen when
  // unwrapped genuinely isn't viable, i.e. fn is explicitly typed to take
  // future<T>& - an explicit opt-in the caller has to write, not a shape
  // that falls out of generic code by accident.
  //
  // The returned future's value type is fn's return type U, unless U is
  // itself a future<V> - futures are monadic, so a continuation that
  // returns a future is flattened into that future<V> directly rather
  // than producing a future<future<V>> a caller would have to unwrap
  // again themselves. Either way, an exception fn throws (or, in the
  // flattened case, that the inner future<V> fails with) is caught here
  // and routed into the returned future via set_exception() instead of
  // escaping - a chained continuation's failure is isolated to its own
  // downstream future and does not stop a sibling continuation queued
  // behind it from running. Never runs inline on whichever call stack
  // completes this future_state: est::loop always defers actually
  // invoking it to its own ready-queue drain pass instead - the returned
  // future<U> reuses this future_state's own loop, so a chain of then()
  // calls all resolve on that one loop.
  // Only current_allocator() is needed to build the downstream state and
  // its node - current_loop() itself is never resolved here;
  // set_continuation() below resolves it fresh on its own, only if this
  // future_state already turns out to be ready.
  //
  // Unwrapped mode's non-void, non-failed case has one more branch on
  // top of this, for an Fn also invocable with T&& (by value, by
  // const T&, or by T&&/auto&& itself - excludes a purely lvalue-bound
  // callback like `[](auto& value)`, which stays on the plain reference
  // path unconditionally): `concrete_continuation<Fn, U>::run()` (below)
  // moves the stored value out, instead of just reading a reference into
  // it, when its own `shared_ptr<future_state<T>>::count() == 1` -
  // nothing else can be left holding this future_state at that point, so
  // there's nothing left to strand with a moved-from value.
  // `future<T>::then(Fn&&) &&` (the rvalue-qualified overload) exists
  // specifically to make that count come back 1 more often, by releasing
  // the calling handle's own reference immediately instead of leaving it
  // held until the handle itself goes out of scope. Issue #64 (the move)
  // / #71 (the && overload).
  // prio defaults to current_priority() (est:loop) - read at the call
  // site, not inside then_impl() - so "no priority given" means "inherit
  // whatever's ambient right now" (issue #31): loop::run_one() sets that
  // ambient value to the currently-running node's own priority for the
  // duration of its run(), so a chain of then() calls inherits by default
  // without threading a parameter through every intermediate call.
  template <detail::then_callback_for<T> Fn>
  auto then(Fn&& fn, Priority prio = current_priority()) {
    return then_impl(std::forward<Fn>(fn), /*run_inline_if_ready=*/false, prio);
  }

  // then_fast(): identical to then() above (same wrapped/unwrapped
  // dispatch, monadic flatten, and move optimization) except a
  // continuation registered on an *already-ready* future_state runs
  // immediately, on the caller's own call stack, instead of always being
  // deferred through est::loop's ready-queue - the same "don't wait for
  // something that isn't being waited for" fast path
  // future_awaiter<T>::await_ready() (below) already gives co_await,
  // extended to then()'s non-coroutine callers. Issue #65: the two were
  // "subtly different" before this existed - an already-ready future
  // resumes a co_await inline, but a then() on one always deferred.
  //
  // An opt-in method, not then()'s own new default: then() must stay
  // safe for a chain of any length (deferring every completion through
  // the loop turns unbounded chain length into an iterative drain
  // instead of direct recursion on the call stack - see
  // set_continuation()'s own doc comment above). then_fast() is for a
  // caller that specifically knows fn is cheap and wants the
  // already-signaled case (say, counting_event<Mode>::wait()'s own
  // already-signaled fast path) to resolve without an extra loop round
  // trip, accepting the same recursion-depth responsibility a
  // coroutine's already-ready co_await always had.
  //
  // Only the direct registration below opts in - a then_fast() callback
  // that itself returns a future<U> still flattens via the ordinary,
  // always-deferred fulfill()/flatten_forwarder<U> path (below): keeping
  // "fast" from also having to thread through the monadic-flatten case
  // keeps this addition to a single call site's behavior, not a second
  // property every future-returning path in this class has to carry.
  template <detail::then_callback_for<T> Fn>
  auto then_fast(Fn&& fn, Priority prio = current_priority()) {
    return then_impl(std::forward<Fn>(fn), /*run_inline_if_ready=*/true, prio);
  }

private:
  // Shared implementation behind then()/then_fast() above - identical in
  // every way except whether a continuation registered on an
  // already-ready future_state runs inline or defers through est::loop;
  // see set_continuation()'s own run_inline_if_ready parameter.
  template <class Fn> auto then_impl(Fn&& fn, bool run_inline_if_ready, Priority prio) {
    using decayed_fn = std::decay_t<Fn>;
    using downstream_value_type = detail::unwrap_future_t<raw_result_t<decayed_fn>>;
    auto allocator = current_allocator();
    auto downstream = shared_ptr<future_state<downstream_value_type>>::make(allocator);
    auto downstream_for_node = downstream; // copy: the node keeps its own reference too
    using node_type = concrete_continuation<decayed_fn, downstream_value_type>;
    auto* node = new node_type(std::forward<Fn>(fn), std::move(downstream_for_node));
    node->priority_level = prio;
    set_continuation(*node, run_inline_if_ready);
    return future<downstream_value_type>(std::move(downstream));
  }

  // Fn's raw (pre-flatten) result type, dispatching wrapped-vs-unwrapped
  // exactly as then() itself does above. A plain (non-consteval-required,
  // never actually called - only ever named inside decltype()) function
  // rather than a single std::conditional_t expression: conditional_t
  // instantiates *both* of its type arguments unconditionally, and for
  // T=void the unwrapped-with-a-value alternative names the ill-formed
  // "const void&" - if constexpr, unlike conditional_t, discards the
  // untaken branch instead of instantiating it.
  //
  // No static_assert here for an Fn that matches neither shape - then()'s
  // own detail::then_callback_for<T> constraint on Fn already rules that
  // out before this is ever instantiated, with a clearer "constraints
  // not satisfied" diagnostic right at the then() call site instead of
  // one buried in here.
  template <class Fn> static consteval auto raw_result_type_tag() {
    if constexpr (std::is_void_v<T>) {
      if constexpr (std::invocable<Fn&>) {
        return std::type_identity<std::invoke_result_t<Fn&>>{};
      } else {
        return std::type_identity<std::invoke_result_t<Fn&, future<T>&>>{};
      }
    } else if constexpr (std::invocable<Fn&, const T&>) {
      return std::type_identity<std::invoke_result_t<Fn&, const T&>>{};
    } else {
      return std::type_identity<std::invoke_result_t<Fn&, future<T>&>>{};
    }
  }
  template <class Fn> using raw_result_t = decltype(raw_result_type_tag<Fn>())::type;

  // Wraps a then() callback together with the downstream future_state it
  // reports its result (or exception) to. U is the downstream's value
  // type - already flattened out of Fn's raw future<U> result, if any -
  // computed once by then() above and reused here so this class doesn't
  // need to repeat that dispatch.
  template <class Fn, class U>
  class concrete_continuation final
      : public continuation_node,
        public detail::current_allocator_new_delete<concrete_continuation<Fn, U>> {
  public:
    concrete_continuation(Fn fn, shared_ptr<future_state<U>> downstream)
        : fn_(std::move(fn)), downstream_(std::move(downstream)) {}

    // Reads owner_ (continuation_node<T>) directly rather than taking a
    // future_state<T>& parameter - see continuation_node<T>'s own doc
    // comment on why run() lives here now instead of behind a second,
    // separately virtual invoke().
    void run() final {
      auto& state = *this->owner_;
      // __cpp_exceptions gate: same reasoning as flatten_forwarder<T>::
      // run()'s own comment above - fn_ genuinely can throw when
      // exceptions are enabled (that's the entire point of this
      // try/catch: propagating it to downstream_), but -fno-exceptions
      // makes `throw` illegal everywhere in the TU, fn_'s own body
      // included, so nothing reaches this catch on such a build and the
      // try/catch becomes dead code the compiler won't even let this
      // class spell.
#ifdef __cpp_exceptions
      try {
#endif
        if constexpr (detail::invocable_unwrapped<Fn, T>()) {
          if (state.ready_with_failure()) {
            // Unwrapped mode's auto-propagate-on-failure, fn_ not called:
            // ready_with_failure() already tells us there's an exception waiting, so
            // fetching it via get_exception() is a plain pointer copy -
            // cheaper than the alternative of calling get() purely to
            // have it rethrow into the catch below, even though this
            // isn't the happy path either way.
            downstream_->set_exception(state.get_exception());
          } else if constexpr (std::is_void_v<T>) {
            invoke_and_fulfill();
          } else if constexpr (std::invocable<Fn&, T&&>) {
            // Only reachable when Fn also accepts an rvalue T (by value,
            // by const T&, or by T&&/auto&& itself) - a generic callback
            // that only binds an lvalue (e.g. `[](auto& value)`, which
            // invocable_unwrapped<Fn, T>() above already confirmed is
            // callable with `const T&`) is std::invocable<Fn&, const T&>
            // but *not* std::invocable<Fn&, T&&>, so it stays on the
            // state.get() path in the else branch below unconditionally -
            // moving from `state` would silently hand it a dangling
            // reference otherwise, since it means to observe the value in
            // place, not take a copy of it.
            if (this->owner_.count() == 1) {
              // this->owner_ is the only shared_ptr<future_state<T>> left
              // (issue #64): no other future<T>/promise<T> handle, and no
              // sibling continuation registered alongside this one, can
              // still be holding `state` - this node is the sole reason it
              // survived this long, and it's about to run() exactly once,
              // right here. Safe, then, to move the stored value out
              // instead of just reading a reference into it - harmless for
              // an Fn taking `const T&` (a const reference binds to an
              // rvalue exactly like an lvalue) and turns a copy into a
              // move for an Fn taking T by value.
              invoke_and_fulfill(std::move(state).get());
            } else {
              invoke_and_fulfill(state.get());
            }
          } else {
            invoke_and_fulfill(state.get());
          }
        } else {
          // Wrapped mode: unwrapped isn't viable for Fn (checked first,
          // above - see then()'s own doc comment on precedence, issue
          // #39), so then_callback_for<Fn, T> guarantees Fn is invocable
          // with future<T>& instead. A fresh future<T> per invocation,
          // not a stored one - this continuation only runs once, so
          // there's nothing to reuse it for, and future_state<T> itself
          // never keeps a future<T> alive on its own account.
          future<T> view(state.shared_from_this());
          invoke_and_fulfill(view);
        }
#ifdef __cpp_exceptions
      } catch (...) {
        downstream_->set_exception(std::current_exception());
      }
#endif
    }

    // Called only when run() never invoked fn_ at all: this node's
    // upstream future_state<T> - the one then() was called on - was
    // itself abandoned (its owning future_state/loop torn down, or this
    // node still sitting unrun in est::loop's own ready_/pending_timers_
    // at teardown) before ever completing. downstream_ must still be
    // completed here, not silently dropped, for the identical reason
    // every other resume node in this codebase (future_resume_node<T>,
    // further down this file; detail::promise_resume_node<T>, est:promise)
    // already completes an abandoned promise instead of just dropping
    // it: a coroutine co_await-ing the future<U> this then() call
    // returned holds that same future_state<U> alive across its own
    // suspension (spilled into its frame - see future_resume_node<T>'s
    // own doc comment), reachable only through this node's downstream_
    // reference until something completes it. Originally left as an open
    // question ("a real, separate question this class doesn't yet
    // answer") until est::mutex::lock()'s own then()-based slow path
    // (issue #67) turned it from a theoretical gap into a real,
    // test-caught leak - a coroutine co_await-ing mutex_ref.lock() left
    // permanently stranded when the mutex (and its underlying
    // future_state<void>) were torn down before that lock() ever
    // resolved. detail::abandoned_exception (est:loop) - shared with
    // every other abandon() override in this codebase that needs to
    // actually complete something, rather than one hand-rolled literal
    // per call site.
    void abandon() noexcept override {
      downstream_->set_exception(std::make_exception_ptr(detail::abandoned_exception()));
    }

    // operator new/delete inherited from current_allocator_new_delete<T>
    // (est:util.current_loop) - see that class's own doc comment for why
    // every concrete ready_node/timer_node needs its own pair rather than
    // one shared at the ready_node/timer_node base itself.

  private:
    // Invokes fn_ with the given arguments (state, a value, or nothing)
    // and reports the result to downstream_ - as a plain value via
    // set_value(), by flattening if it's itself a future (see fulfill()
    // below), or, if fn_'s return type is void, via downstream_'s own
    // no-argument set_value() (only valid when U is void, which is
    // exactly the case then() arranges for a void-returning fn).
    template <class... Args> void invoke_and_fulfill(Args&&... args) {
      if constexpr (std::is_void_v<std::invoke_result_t<Fn&, Args...>>) {
        fn_(std::forward<Args>(args)...);
        downstream_->set_value();
      } else {
        fulfill(fn_(std::forward<Args>(args)...));
      }
    }

    // Reports a non-void fn_ result to downstream_: directly via
    // set_value() if it's a plain U, or - if it's itself a future<U> - by
    // registering a detail::flatten_forwarder<U> node directly on the
    // inner future's own future_state<U>: no callback, no closure, and
    // no intermediate future<U>/future_state<U> pair to allocate and
    // immediately discard - result.state_ is reached directly via future<U>'s
    // `template <class> friend class future_state;` declaration, since
    // this code is itself nested inside a future_state<T> instantiation
    // and nested-class members share their enclosing class's access
    // rights). U, not inner_value_type, throughout: then() already
    // computed downstream_'s value type by unwrapping Fn's raw future<U>
    // result, so the two are always the same type here.
    template <class R> void fulfill(R&& result) {
      if constexpr (detail::is_future_v<std::decay_t<R>>) {
        auto* node = new detail::flatten_forwarder<U>(downstream_);
        result.state_->set_continuation(*node);
      } else {
        downstream_->set_value(std::forward<R>(result));
      }
    }

    Fn fn_;
    shared_ptr<future_state<U>> downstream_;
  };

  // Precondition, not a recoverable error: set_value()/set_exception()
  // must each be called at most once. Debug-only (unlike std::promise,
  // which throws) - a release build that violates this silently
  // overwrites result_ and, for any continuations that already ran off
  // the first completion, delivers a value/exception they never see.
  // Revisit if that turns out to matter in practice; not changing it
  // speculatively now. Called by each setter *before* its own emplace
  // (not from complete(), which runs after - by then ready() is
  // unconditionally true) - factored out just to keep the same message
  // in one place across all three setters.
  void check_not_completed() { check(!ready(), "future_state completed more than once"); }

  // Hands every registered continuation off to est::loop's ready-queue
  // (normally at most one - future is meant to be a single-consumer
  // handle - but nothing stops a caller from registering more than one
  // via then(), and the underlying list already supports it; drained in
  // est::intrusive_list's documented FIFO order). Called by each setter
  // after it has already stored the result into result_ - this function
  // only drains, it doesn't know or care what was stored. Each node
  // binds a fresh shared_ptr back to this future_state right before
  // being handed off (see continuation_node<T>::bind_owner()'s own doc
  // comment on why not earlier), so this future_state is guaranteed to
  // survive until est::loop actually runs (and destroys) it, even if
  // every other reference to it (promise, future) is dropped in the
  // meantime.
  void complete() {
    auto& loop_ref = current_loop();
    waiters_.drain([this, &loop_ref](detail::ready_node& node) {
      // Safe by construction: every node ever enqueued here arrived
      // through set_continuation(continuation_node&) below, never
      // anything else, so it's always actually a continuation_node -
      // exactly the same "safe by construction, not by RTTI" contract
      // est::intrusive_list<T>::dequeue() itself already documents, one
      // level further out since waiters_ is keyed on ready_node rather
      // than continuation_node specifically (see waiters_'s own doc
      // comment on why).
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
      auto& typed_node = static_cast<continuation_node&>(node);
      typed_node.bind_owner(this->shared_from_this());
      loop_ref.enqueue_ready(typed_node);
    });
  }

  // detail::ready_node, not continuation_node (the more specific type
  // set_continuation() below actually enqueues) - deliberately: this
  // future_state<T> is what continuation_node<T>::owner_ itself holds a
  // shared_ptr back to, so instantiating continuation_node<T> to store it
  // here would force shared_ptr<future_state<T>>'s intrusive
  // specialization to resolve std::derived_from<future_state<T>,
  // ref_counted> - requiring future_state<T> complete - while
  // future_state<T> is still busy being defined (this very member).
  // ready_node (est:loop) is already complete by this point regardless of
  // T, so keying waiters_ on it instead sidesteps that cycle entirely;
  // complete() recovers the real continuation_node type itself where it
  // actually needs it (bind_owner()) - the destructor above needs no such
  // downcast, since abandon()/delete both work through the plain
  // ready_node& it already has. continuation_node<T> itself is untouched
  // by this - it still declares
  // a plain shared_ptr<future_state<T>> owner_ member, same as ever;
  // nothing here forces it to be instantiated before future_state<T>
  // completes any more, so that member now resolves fine wherever
  // continuation_node<T> actually first gets used (concrete_continuation
  // below, future_resume_node<T> further down this file) - always after
  // this class's own definition has finished.
  intrusive_list<detail::ready_node> waiters_;
  std::variant<std::monostate, stored_t, std::exception_ptr> result_;
};

} // namespace est

namespace est::detail {

// The return_value()/return_void() half of future<T>::promise_type
// (below), split into its own base so a coroutine returning
// est::future<T> gets exactly one of the two - a promise_type providing
// both is a compile error, and which one a `co_return` needs depends on
// whether T is void, exactly the fork future_state<T>::stored_t/
// set_value() already resolves via std::conditional_t/if constexpr
// rather than this file duplicating future<T>'s entire promise_type as
// two near-identical T/void specializations.
template <class T> class future_promise_result {
public:
  void return_value(const T& value) { state_->set_value(value); }
  void return_value(T&& value) { state_->set_value(std::move(value)); }

protected:
  explicit future_promise_result(shared_ptr<future_state<T>> state) : state_(std::move(state)) {}
  // Protected, not private-with-an-accessor: this base exists solely for
  // future<T>::promise_type (below) to inherit from, and promise_type
  // itself needs direct access to state_ (get_return_object(),
  // unhandled_exception(), operator new/delete all touch it) - the
  // accessor a private member would need is exactly this field, with
  // extra ceremony and no actual encapsulation gained, since promise_type
  // is this class's only ever derived type.
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  shared_ptr<future_state<T>> state_;
};

template <> class future_promise_result<void> {
public:
  void return_void() { state_->set_value(); }

protected:
  explicit future_promise_result(shared_ptr<future_state<void>> state) : state_(std::move(state)) {}
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  shared_ptr<future_state<void>> state_;
};

} // namespace est::detail

export namespace est {

// Consumer handle: a thin, move-only view over a future_state<T>, backed
// by an est::shared_ptr so destroying a future does not destroy the
// future_state if something else - a still-live est::promise, or a
// continuation node already handed off to est::loop's ready-queue (see
// continuation_node<T>::bind_owner()) - still references it.
template <class T> class future {
public:
  explicit future(shared_ptr<future_state<T>> state) noexcept : state_(std::move(state)) {}
  future(const future&) = delete;
  auto operator=(const future&) -> future& = delete;
  future(future&&) noexcept = default;
  auto operator=(future&&) noexcept -> future& = default;
  ~future() = default;

  [[nodiscard]] auto ready() const noexcept -> bool { return state_->ready(); }

  [[nodiscard]] auto ready_with_failure() const noexcept -> bool {
    return state_->ready_with_failure();
  }

  // See future_state<T>::ready_with_value()'s own doc comment - one call
  // in place of `ready() && !ready_with_failure()`, most useful right
  // after a when_any()-style race to find out which future actually won
  // *and* won cleanly.
  [[nodiscard]] auto ready_with_value() const noexcept -> bool {
    return state_->ready_with_value();
  }

  // Returns the stored exception_ptr directly, without going through
  // get()'s throw/rethrow. Precondition: ready_with_failure(). Pairs
  // with ready_with_failure() for a caller that already knows there's an
  // exception waiting and wants to forward or inspect it without paying
  // for a throw/catch round-trip just to retrieve a pointer.
  [[nodiscard]] auto get_exception() const noexcept -> std::exception_ptr {
    return state_->get_exception();
  }

  // Deducing this: future.get() (lvalue) copy-constructs from the
  // future_state's non-consuming get(); std::move(future).get() forwards
  // its rvalue-ness through, so the future_state's value is moved
  // directly into the return value instead of copied - safe here
  // specifically because a future is a single-consumer handle, unlike
  // future_state<T>::get() itself, which multiple then() continuations
  // may each call.
  //
  // self isn't itself forwarded (NOLINTNEXTLINE below): state_ is a
  // shared_ptr member, and forwarding it doesn't propagate value
  // category to what it points to the way it would for a value/reference
  // member - the if constexpr branch is what actually turns *state_'s
  // lvalue-ness into an xvalue for the rvalue-self case.
  // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
  template <class Self> [[nodiscard]] auto get(this Self&& self) -> T {
    if constexpr (std::is_lvalue_reference_v<Self>) {
      return self.state_->get();
    } else {
      return std::move(*self.state_).get();
    }
  }

  // Returns another future<T> aliasing the same future_state as *this -
  // both see the same eventual result, and either may safely call
  // get() or register any number of then() callbacks, any number of
  // times. Just a copy of state_ - cheap regardless of whether T
  // happens to opt into est::ref_counted's intrusive counting or the
  // default control_block-based one (est:util.shared_ptr): either way
  // it's a plain refcount bump, no extra allocation.
  //
  // Constrained to T = void or a scalar T (integral, floating-point,
  // enumeration, pointer, pointer-to-member, or std::nullptr_t) rather
  // than offered for every T, because of what a clone does *not* make
  // safe in general: future<T>::get()'s consuming (rvalue) path - the
  // one co_await always takes (this class's own operator co_await(),
  // below) - called from more than one clone of the same future_state.
  // The second such call would read the first's moved-from leftovers,
  // silently: future_state<T>::get()'s lvalue branch copies the stored
  // value out (harmless, repeatable - what then()'s own unwrapped
  // dispatch already relies on to let multiple continuations each read
  // it), but the rvalue branch moves it out, and a move doesn't reset
  // the underlying storage - it just leaves whatever moved-from state T
  // ends up in sitting there permanently. That hazard needs T to have a
  // meaningfully different "moved-from" state to begin with. For T =
  // void there's nothing to consume in the first place; for a scalar T,
  // move construction/assignment is defined to do exactly what copying
  // it would - it reads the value, and the "moved-from" object is left
  // completely unchanged (unlike, say, std::string or std::vector) - so
  // every clone consuming the same scalar via co_await/rvalue get() is
  // just redundantly reading the same, still-intact value, not a bug.
  // Every other T keeps this as a caller-enforced precondition instead
  // (at most one clone may ever be co_await-ed or rvalue-get()-ed) -
  // that's real and not caught by this constraint, but the constraint
  // at least removes the two cases where the whole hazard was moot to
  // begin with.
  [[nodiscard]] auto clone() const -> future
    requires(std::is_void_v<T> || std::is_scalar_v<T>)
  {
    return future(state_);
  }

  // Forwards to future_state<T>::then() (see its own doc comment) - the
  // node allocation and registration live there now, not here. Lvalue-
  // qualified so it can coexist with the rvalue-qualified overload right
  // below (a member function can't mix a ref-unqualified and a ref-
  // qualified overload of the same signature - a real lvalue future<T>
  // handle is the only caller that needs this one; an rvalue always binds
  // the other overload instead).
  template <class Fn> auto then(Fn&& fn, Priority prio = current_priority()) & {
    return state_->then(std::forward<Fn>(fn), prio);
  }

  // Same, for a caller that no longer needs *this once the continuation
  // is registered (`std::move(future).then(fn)`; a plain temporary -
  // `future_returning_call().then(fn)` - already reaches this overload
  // too, since a prvalue is an rvalue). Releases this handle's own
  // reference to the future_state immediately, rather than leaving it
  // held until *this goes out of scope: every reference this handle
  // would otherwise keep alive past this call is one more reason
  // `concrete_continuation<Fn, U>::run()`'s own `count() == 1` check
  // (issue #64, `future_state<T>::then()`'s own doc comment) could come
  // back false. Issue #71.
  template <class Fn> auto then(Fn&& fn, Priority prio = current_priority()) && {
    auto state = std::move(state_);
    return state->then(std::forward<Fn>(fn), prio);
  }

  // Forwards to future_state<T>::then_fast() (see its own doc comment) -
  // the same lvalue/rvalue-qualified split as then() above, for API
  // shape consistency, but *not* for the same payoff: issue #64/#71's
  // move optimization (concrete_continuation<Fn, U>::run()'s own
  // `owner_.count() == 1` check) can never actually fire through this
  // overload the way it does for then(). then_fast() runs the
  // continuation synchronously, inside this very function call - the
  // `state` local right below is still alive (it's this call's own
  // stack frame) at the moment run() checks owner_.count(), contributing
  // a reference on top of owner_'s own, so the count this checks is at
  // least 2, never 1. then()'s version of this optimization only reaches
  // 1 because its run() happens *later*, via est::loop's drain, by which
  // point a temporary exactly like this one has already gone out of
  // scope. Kept anyway so a caller that no longer needs *this can still
  // say so, same as with then() - it just isn't what makes the
  // difference here.
  template <class Fn> auto then_fast(Fn&& fn, Priority prio = current_priority()) & {
    return state_->then_fast(std::forward<Fn>(fn), prio);
  }

  template <class Fn> auto then_fast(Fn&& fn, Priority prio = current_priority()) && {
    auto state = std::move(state_);
    return state->then_fast(std::forward<Fn>(fn), prio);
  }

  // Suspends the calling coroutine until *this becomes ready, resuming
  // with its value (or rethrowing its exception) - the primitive that
  // lets one coroutine `co_await` any est::future<T>, whatever produced
  // it: est::sleep_for()/sleep_until(), a then() chain, or another
  // coroutine returning est::future<U> (this class's own promise_type,
  // below). Defined out of line, after detail::future_awaiter<T> - see
  // that class's own doc comment on why.
  //
  // No ref-qualifier: callable whether *this is a named lvalue
  // (`co_await someFuture;`) or a prvalue temporary (`co_await
  // sleep_for(...);`) - the language extends the latter's lifetime
  // across the whole co_await expression, including through suspension,
  // so a reference to `*this` stays valid inside the returned awaiter
  // either way.
  [[nodiscard]] auto operator co_await() noexcept -> detail::future_awaiter<T>;

  // The compiler-generated glue for a coroutine whose return type is
  // est::future<T> - `est::future<int> foo(...) { co_await ...; co_return
  // 42; }`. Deliberately not a separate task<T> wrapper type: the whole
  // point is that a caller of foo() gets back a plain est::future<int>,
  // indistinguishable from one built out of sleep_for()/then() chains -
  // nothing about future<T>'s own interface changes; this class only
  // exists for the compiler's coroutine machinery to find via the
  // standard promise_type protocol.
  //
  // Always built against est::current_loop() (est:util.current_loop),
  // whatever parameters the coroutine function itself takes - there is
  // no way to pass this class an explicit `loop&` at all. The single
  // constructor/operator new pair below is templated on an arbitrary
  // (possibly empty) `Args&...` pack purely so it matches whatever
  // parameter list the actual coroutine function declares, per the
  // standard's "promise constructor arguments" rule (the compiler tries
  // building promise_type from the coroutine call's own argument list
  // before ever falling back to a default constructor) - the arguments
  // themselves are never read.
  class promise_type : public detail::future_promise_result<T> {
  public:
    template <class... Args>
    explicit promise_type(Args&... /*unused*/) : detail::future_promise_result<T>(make_state()) {}

    promise_type(const promise_type&) = delete;
    auto operator=(const promise_type&) -> promise_type& = delete;
    promise_type(promise_type&&) = delete;
    auto operator=(promise_type&&) -> promise_type& = delete;
    ~promise_type() = default;

    auto get_return_object() -> future { return future(this->state_); }

    // Never suspends at the start: the coroutine's synchronous prefix (up
    // to its first genuine suspension point, if any) runs immediately, on
    // the caller's own stack, exactly like the work an ordinary function
    // does before handing back a future - see future_awaiter<T>::
    // await_ready()'s own doc comment (below) for the matching decision
    // on the other end of a co_await. Suspending unconditionally instead
    // would cost one extra heap-allocated resume node and ready-queue
    // round trip per coroutine call, even for one that never awaits
    // anything at all.
    auto initial_suspend() noexcept -> std::suspend_never { return {}; }

    // Never suspends at the end either: nothing outside this coroutine
    // holds (or needs) a coroutine_handle to it - only the future<T>
    // returned by get_return_object() (a shared_ptr<future_state<T>>
    // underneath), which by now already has its result via
    // return_value()/return_void()/unhandled_exception(). std::suspend_never
    // here lets the compiler destroy the coroutine frame immediately and
    // automatically once the body finishes. Safe to do so unconditionally:
    // every node that ever resumes this coroutine (future_resume_node<T>
    // below) is separately heap-allocated, entirely independent of the
    // frame this suspend point destroys - see future_resume_node<T>'s
    // own doc comment for
    // why it has to be heap-allocated rather than embedded in the frame
    // it resumes - so there is nothing left in this frame for anything
    // to touch afterward.
    auto final_suspend() noexcept -> std::suspend_never { return {}; }

    void unhandled_exception() { this->state_->set_exception(std::current_exception()); }

    // pmr-aware coroutine frame allocation - see
    // detail::coroutine_frame_alloc()/coroutine_frame_dealloc()'s own
    // doc comment. Templated on an arbitrary (possibly empty) Args&...
    // pack for the same "promise constructor arguments" reason the
    // constructor above is - always allocates against est::current_allocator().
    template <class... Args>
    static auto operator new(std::size_t size, Args&... /*unused*/) -> void* {
      return detail::coroutine_frame_alloc(size, current_allocator());
    }

    static void operator delete(void* ptr, std::size_t size) noexcept {
      detail::coroutine_frame_dealloc(ptr, size);
    }

  private:
    static auto make_state() -> shared_ptr<future_state<T>> {
      return shared_ptr<future_state<T>>::make(current_allocator());
    }
  };

private:
  // Grants every future_state<U> instantiation access to state_ below -
  // specifically for detail::flatten_forwarder<T>'s one caller,
  // future_state<T>::concrete_continuation<Fn, U>::fulfill() (:future's
  // own future_state<T> definition, above), which needs to register a
  // continuation directly on an inner future<T>'s own future_state<T>
  // without going through any future<T> method (there is deliberately no
  // public one for this - see fulfill()'s own doc comment). A nested
  // class shares its enclosing class's access rights, so this one
  // `friend` declaration is all every future_state<T> instantiation
  // needs, not one per (T, U) pair.
  template <class> friend class future_state;

  friend class detail::future_awaiter<T>;
  shared_ptr<future_state<T>> state_;
};

} // namespace est

namespace est::detail {

// A plain resumption trampoline for a coroutine awaiting an est::future<T>,
// used only on the genuinely-not-ready path (future_awaiter<T>::
// await_ready() below returns true, skipping this node entirely, when the
// awaited future is already resolved by the time co_await evaluates it).
// Separately heap-allocated via an allocator (like every other ready_node
// this codebase queues - concrete_continuation<Fn, U>,
// est:promise's sleep_resume_node/promise_resume_node<T>), *not* embedded
// inside the coroutine frame it resumes: an awaiter object embedded in a
// coroutine's own frame only lives for the duration of *its own*
// co_await expression - once run() resumes the coroutine past that
// point, the compiler is free to reuse that exact frame storage for
// whatever the coroutine's later code constructs (its next awaiter, a
// local variable, ...), since their lifetimes don't overlap. But
// est::loop::run_one() (est:loop) deletes this same node *after* run()
// already returned - for a frame-embedded node, that storage could
// already be overwritten by then, making a virtual call through it
// undefined behavior. A separately allocated node has its own real,
// independent lifetime, so run()-then-delete is exactly as safe here as
// it already is for every other ready_node in this codebase.
template <class T>
class future_resume_node final : public continuation_node<T>,
                                 public current_allocator_new_delete<future_resume_node<T>> {
public:
  explicit future_resume_node(std::coroutine_handle<> handle) noexcept : handle_(handle) {}

  void run() final { handle_.resume(); }

  // Called only when run() never ran (the future_state this node was
  // registered on was dropped without ever completing, per
  // ready_node::abandon()'s own doc comment, est:loop) - the awaiting
  // coroutine is still fully intact and untouched, so this is the only
  // chance to free its frame. Never called once run() has run: the
  // coroutine either already self-destroyed or suspended again on
  // something else that now owns it, and touching handle_ then would be
  // wrong either way.
  void abandon() noexcept override { handle_.destroy(); }

  // operator new/delete inherited from current_allocator_new_delete<T>
  // (est:util.current_loop) - see that class's own doc comment for why
  // every concrete ready_node/timer_node needs its own pair rather than
  // one shared at the ready_node/timer_node base itself.

private:
  std::coroutine_handle<> handle_;
};

// Awaiter for `co_await someFuture` on any est::future<T> - see
// future<T>::operator co_await() above. *this itself is a coroutine-frame
// subobject - fine here for the identical reason future_resume_node<T>'s
// own doc comment gives for *not* being one itself: await_suspend()
// below hands the actual resumption off to a separately heap-allocated
// future_resume_node<T> before this awaiter's own co_await expression
// ends, and nothing reaches back into *this afterward.
template <class T> class future_awaiter {
public:
  explicit future_awaiter(future<T>& fut) noexcept : future_(fut) {}
  // Deleted, not defaulted - never actually invoked: every use of this
  // class returns a genuine prvalue future<T>::operator co_await()
  // elides into place, the same guaranteed-elision pattern
  // est::scope_exit's own doc comment (est:util.scope_exit) already
  // documents relying on.
  future_awaiter(const future_awaiter&) = delete;
  auto operator=(const future_awaiter&) -> future_awaiter& = delete;
  future_awaiter(future_awaiter&&) = delete;
  auto operator=(future_awaiter&&) -> future_awaiter& = delete;
  ~future_awaiter() = default;

  // Skips suspension entirely when the awaited future is already
  // resolved - the same "don't wait for something that isn't being
  // waited for" reasoning promise_type::initial_suspend() (above) uses
  // at the other end of a coroutine's lifetime: paying for a
  // future_resume_node<T> allocation and a full ready-queue round trip
  // purely to resume something that was never actually going to wait for
  // anything isn't worth it. future_state<T>::set_continuation() (called
  // from await_suspend() below) still handles the "not yet ready" case
  // correctly either way - this only changes whether that call, and the
  // node it needs, happens at all.
  [[nodiscard]] auto await_ready() const noexcept -> bool { return future_.ready(); }

  // node->priority_level stamped from current_priority() explicitly, not
  // left at ready_node's own Priority::normal default: this is the other
  // half of issue #31's inheritance (then()/then_fast()'s own `prio =
  // current_priority()` default argument is the first) - a coroutine
  // resumed at some priority that then co_awaits something should have
  // *that* resumption also run at the same priority by default, without
  // needing to pass it anywhere - co_await's own syntax has no room for
  // an extra argument the way then()/then_fast() do.
  void await_suspend(std::coroutine_handle<> handle) {
    auto* node = new future_resume_node<T>(handle);
    node->priority_level = current_priority();
    future_.state_->set_continuation(*node);
  }

  // Moves the value out rather than copying it: co_await is inherently a
  // single-consumption use of whatever it's awaiting (the awaited value
  // only exists at this one point in the awaiting coroutine's control
  // flow) - the same reasoning future<T>::get()'s own && overload
  // already documents. Propagates (rather than returning) a stored
  // exception: get() rethrows on failure, which - thrown from inside
  // await_resume() - the language defines as equivalent to throwing from
  // the co_await expression itself, so it surfaces in the awaiting
  // coroutine exactly like any other exception crossing a co_await.
  auto await_resume() -> T { return std::move(future_).get(); }

private:
  future<T>& future_;
};

} // namespace est::detail

namespace est {

template <class T> auto future<T>::operator co_await() noexcept -> detail::future_awaiter<T> {
  return detail::future_awaiter<T>(*this);
}

} // namespace est
