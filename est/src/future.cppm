export module est:future;

import std;
import :check;
import :loop;
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
// the one thing that actually needs T (invoke()), plus the plumbing
// run() needs to call it once est::loop actually dequeues and runs this
// node. Not nested inside future_state<T> - it's an implementation
// detail of :future, not part of future_state's public surface, so it
// lives here instead.
template <class T> class continuation_node : public detail::ready_node {
public:
  void run() final { invoke(*owner_); }

  virtual void invoke(future_state<T>& state) = 0;

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

private:
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
// alone (issue #25's own follow-up simplification, replacing an earlier
// on_ready()/raw_continuation<Fn> pair that were templated on an arbitrary
// callback type instead) - every flatten at the same T reuses this one
// instantiation instead of minting a fresh one per (T, Fn, U) call site.
template <class T> class flatten_forwarder final : public continuation_node<T> {
public:
  explicit flatten_forwarder(shared_ptr<future_state<T>> downstream)
      : downstream_(std::move(downstream)) {}

  void invoke(future_state<T>& state) override {
    if (state.failed()) {
      downstream_->set_exception(state.get_exception());
      return;
    }
    try {
      if constexpr (std::is_void_v<T>) {
        downstream_->set_value();
      } else {
        downstream_->set_value(std::move(state).get());
      }
    } catch (...) {
      downstream_->set_exception(std::current_exception());
    }
  }

  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept override {
    allocator.delete_object(this);
  }

private:
  shared_ptr<future_state<T>> downstream_;
};

// A placeholder "success" alternative for future_state<void>'s result_
// variant: void itself can't be a variant alternative, and reusing
// std::monostate for both "not yet ready" and "ready with no value" would
// make the two states indistinguishable. Never observed by a caller -
// future_state<void>::get() (see below) always returns void, not this
// type.
struct void_value {};

// Pattern-matches Fn's raw then() result against est::future<U> so
// then() can flatten a continuation returning a future into a plain
// future<U> instead of a future<future<U>> - futures are monadic, per
// the redesign in docs/PLAN.md (issue #23). Forward-declared only
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
// invocable_unwrapped<Fn, T>() checked first, not just as a matter of
// style matching then()'s own dispatch order (issue #39) but because it
// has to be: constraint disjunction (||) short-circuits left-to-right, so
// whichever operand comes first is the one actually evaluated for an Fn
// that satisfies it. A generic callback (an `auto&` lambda, say) that's
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
// - a view over shared state, not the state itself (docs/PLAN.md). Not
// exported (see the forward declaration above): a caller never sees
// this type directly, only through the est::promise<T>/est::future<T>
// handles that wrap it - including a then() callback registered with
// the "wrapped" calling convention, which receives a real est::future<T>
// (built via shared_from_this(), below) rather than a future_state<T>&.
// Holds value-or-exception storage and a continuation slot built on
// est::intrusive_list. Lifetime is managed externally by an
// est::shared_ptr<future_state<T>> (see make_promise_future() in
// est:promise) - never constructed directly by a caller, and holds no
// ref count of its own.
//
// Holds a reference to the est::loop it was created against (M3,
// docs/PLAN.md) rather than its own allocator - the allocator it uses is
// simply the loop's (loop_.allocator()), an explicit dependency threaded
// through make_promise_future(loop&) (est:promise) rather than a global
// singleton like est::platform::instance(): unlike platform, a loop
// carries real mutable state (its ready-queue, pending timers) a shared
// global would accumulate cross-test contamination in.
//
// Precondition, for the lifetime of every future_state (and therefore
// every promise<T>/future<T>/then()-chain) built against a given loop:
// that loop must outlive all of them. There is no way to check this at
// runtime the way est::check() guards other preconditions elsewhere in
// this codebase (a dangling reference can't be tested for validity) - a
// caller returning a future/promise from a function whose loop is a
// local variable, or otherwise letting one outlive its loop, gets
// undefined behavior on the very next touch of loop_ (allocator(),
// set_continuation(), even this class's own destructor). Callers must
// arrange loop lifetime themselves; est::loop's own doc comment
// (est:loop) describes the same dependency from the loop's side.
template <class T> class future_state : public enable_shared_from_this<future_state<T>> {
public:
  using continuation_node = detail::continuation_node<T>;

  // What result_ actually stores on success: T itself, except when T is
  // void (variant can't hold void as an alternative), in which case a
  // stand-in tag type is used instead - see detail::void_value.
  using stored_t = std::conditional_t<std::is_void_v<T>, detail::void_value, T>;

  explicit future_state(loop& loop_ref) noexcept : loop_(loop_ref) {}

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
  ~future_state() {
    waiters_.drain([this](continuation_node& node) { node.destroy(loop_.allocator()); });
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

  void set_exception(std::exception_ptr exception) {
    check_not_completed();
    result_.template emplace<std::exception_ptr>(std::move(exception));
    complete();
  }

  // Registers a continuation node (already allocated via allocator()) to
  // run once ready. If already ready, hands it straight to est::loop's
  // ready-queue instead of queueing it locally - either way, this
  // future_state never invokes a continuation itself; est::loop always
  // does, on its own drain pass, never inline on this call stack (M3,
  // docs/PLAN.md).
  void set_continuation(continuation_node& node) {
    if (ready()) {
      node.bind_owner(this->shared_from_this());
      loop_.enqueue_ready(node);
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
  [[nodiscard]] auto failed() const noexcept -> bool {
    return std::holds_alternative<std::exception_ptr>(result_);
  }

  // Returns the stored exception_ptr directly, without going through
  // get()'s throw/rethrow. Precondition: failed(). Pairs with failed()
  // for a caller that already knows there's an exception waiting and
  // wants to forward it without paying for a throw/catch round-trip
  // just to retrieve a pointer - used internally by then()'s
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
  // validate-at-boundaries philosophy). But std::get<T>() below also
  // throws std::bad_variant_access unconditionally, regardless of
  // NDEBUG, as an accidental (not intentionally designed) second line of
  // defense for T != void; every std::get<stored_t> call, including the
  // T = void one, keeps that same accidental behavior rather than
  // letting T = void alone skip it by returning before ever touching
  // result_'s active alternative.
  template <class Self> [[nodiscard]] decltype(auto) get(this Self&& self) {
    check(self.ready());
    if (auto* exception = std::get_if<std::exception_ptr>(&self.result_)) {
      std::rethrow_exception(*exception);
    }
    if constexpr (std::is_void_v<T>) {
      (void)std::get<stored_t>(self.result_);
      return;
    } else {
      return std::get<T>(std::forward<Self>(self).result_);
    }
  }

  [[nodiscard]] auto allocator() const noexcept -> std::pmr::polymorphic_allocator<std::byte> {
    return loop_.allocator();
  }

  // Registers fn to run once ready. Two calling conventions, chosen by
  // how fn can be invoked (docs/PLAN.md, issue #23; precedence changed
  // by issue #39):
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
  //     comment); fn inspects ready()/failed()/get() to decide what to
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
  // invoking it to its own ready-queue drain pass instead (M3,
  // docs/PLAN.md) - the returned future<U> reuses this future_state's
  // own loop, so a chain of then() calls all resolve on that one loop.
  template <detail::then_callback_for<T> Fn> auto then(Fn&& fn) {
    using decayed_fn = std::decay_t<Fn>;
    using downstream_value_type = detail::unwrap_future_t<raw_result_t<decayed_fn>>;
    auto downstream =
        shared_ptr<future_state<downstream_value_type>>::make(loop_.allocator(), loop_);
    auto downstream_for_node = downstream; // copy: the node keeps its own reference too
    using node_type = concrete_continuation<decayed_fn, downstream_value_type>;
    auto* node = loop_.allocator().template new_object<node_type>(std::forward<Fn>(fn),
                                                                  std::move(downstream_for_node));
    set_continuation(*node);
    return future<downstream_value_type>(std::move(downstream));
  }

private:
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
  template <class Fn, class U> class concrete_continuation final : public continuation_node {
  public:
    concrete_continuation(Fn fn, shared_ptr<future_state<U>> downstream)
        : fn_(std::move(fn)), downstream_(std::move(downstream)) {}

    void invoke(future_state& state) override {
      try {
        if constexpr (detail::invocable_unwrapped<Fn, T>()) {
          if (state.failed()) {
            // Unwrapped mode's auto-propagate-on-failure, fn_ not called:
            // failed() already tells us there's an exception waiting, so
            // fetching it via get_exception() is a plain pointer copy -
            // cheaper than the alternative of calling get() purely to
            // have it rethrow into the catch below, even though this
            // isn't the happy path either way.
            downstream_->set_exception(state.get_exception());
          } else if constexpr (std::is_void_v<T>) {
            invoke_and_fulfill();
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
      } catch (...) {
        downstream_->set_exception(std::current_exception());
      }
    }

    // `this` here is concrete_continuation<Fn, U>*, so delete_object
    // deallocates with this type's actual size/alignment - the whole
    // reason destroy() is virtual instead of the caller deallocating
    // through a detail::ready_node& (see that class's own doc comment,
    // est:loop).
    void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept override {
      allocator.delete_object(this);
    }

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
    // inner future's own future_state<U> (issue #25's own follow-up
    // simplification: no callback, no closure, and no intermediate
    // future<U>/future_state<U> pair to allocate and immediately discard -
    // result.state_ is reached directly via future<U>'s
    // `template <class> friend class future_state;` declaration, since
    // this code is itself nested inside a future_state<T> instantiation
    // and nested-class members share their enclosing class's access
    // rights). U, not inner_value_type, throughout: then() already
    // computed downstream_'s value type by unwrapping Fn's raw future<U>
    // result, so the two are always the same type here.
    template <class R> void fulfill(R&& result) {
      if constexpr (detail::is_future_v<std::decay_t<R>>) {
        auto* node = result.state_->allocator().template new_object<detail::flatten_forwarder<U>>(
            downstream_);
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
  // est::intrusive_list's documented LIFO order). Called by each setter
  // after it has already stored the result into result_ - this function
  // only drains, it doesn't know or care what was stored. Each node
  // binds a fresh shared_ptr back to this future_state right before
  // being handed off (see continuation_node<T>::bind_owner()'s own doc
  // comment on why not earlier), so this future_state is guaranteed to
  // survive until est::loop actually runs (and destroys) it, even if
  // every other reference to it (promise, future) is dropped in the
  // meantime.
  void complete() {
    waiters_.drain([this](continuation_node& node) {
      node.bind_owner(this->shared_from_this());
      loop_.enqueue_ready(node);
    });
  }

  loop& loop_;
  intrusive_list<continuation_node> waiters_;
  std::variant<std::monostate, stored_t, std::exception_ptr> result_;
};

} // namespace est

export namespace est {

// Consumer handle: a thin, move-only view over a future_state<T>, backed
// by an est::shared_ptr so destroying a future does not destroy the
// future_state if something else - a still-live est::promise, or a
// continuation node already handed off to est::loop's ready-queue (M3,
// see continuation_node<T>::bind_owner()) - still references it.
template <class T> class future {
public:
  explicit future(shared_ptr<future_state<T>> state) noexcept : state_(std::move(state)) {}
  future(const future&) = delete;
  auto operator=(const future&) -> future& = delete;
  future(future&&) noexcept = default;
  auto operator=(future&&) noexcept -> future& = default;
  ~future() = default;

  [[nodiscard]] auto ready() const noexcept -> bool { return state_->ready(); }

  [[nodiscard]] auto failed() const noexcept -> bool { return state_->failed(); }

  // Returns the stored exception_ptr directly, without going through
  // get()'s throw/rethrow. Precondition: failed(). Pairs with failed()
  // for a caller that already knows there's an exception waiting and
  // wants to forward or inspect it without paying for a throw/catch
  // round-trip just to retrieve a pointer.
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

  // Forwards to future_state<T>::then() (see its own doc comment) - the
  // node allocation and registration live there now, not here.
  template <class Fn> auto then(Fn&& fn) { return state_->then(std::forward<Fn>(fn)); }

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

  shared_ptr<future_state<T>> state_;
};

} // namespace est
