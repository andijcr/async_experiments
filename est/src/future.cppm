export module est:future;

import std;
import :check;
import :sync.mutex;
import :util.scope_exit;
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

// Type-agnostic base for a future_state<T>'s queued continuations: only
// the parts that don't depend on T live here, so they're compiled once
// instead of once per T. future_state<T>'s own destructor (which only
// ever needs to destroy(), never invoke()) operates on this directly.
class waiter_node : public mutex_waiter {
public:
  waiter_node() = default;
  waiter_node(const waiter_node&) = delete;
  auto operator=(const waiter_node&) -> waiter_node& = delete;
  waiter_node(waiter_node&&) = delete;
  auto operator=(waiter_node&&) -> waiter_node& = delete;
  virtual ~waiter_node() = default;

  // Deallocates *this through the *actual* allocated type (each
  // override does `allocator.delete_object(this)` with `this` typed as
  // the concrete class). Calling `allocator.delete_object` on a
  // waiter_node& directly would deduce the base type and deallocate
  // with the base's size/alignment instead of the derived type actually
  // allocated - undefined behaviour per memory_resource::deallocate's
  // precondition that the size/alignment match the original allocate()
  // call, silently "working" with the default new/delete resource but
  // corrupting a pool-style resource.
  virtual void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept = 0;
};

// The T-dependent half of a queued continuation: adds only the one
// thing that actually needs T (invoke()). Not nested inside
// future_state<T> - it's an implementation detail of :future, not part
// of future_state's public surface, so it lives here instead.
template <class T> class continuation_node : public waiter_node {
public:
  virtual void invoke(future_state<T>& state) = 0;
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
template <class Fn, class T>
concept then_callback_for = std::invocable<Fn&, future<T>&> || invocable_unwrapped<Fn, T>();

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
// est::waiter_list. Lifetime is managed externally by an
// est::shared_ptr<future_state<T>> (see make_promise_future() in
// est:promise) - never constructed directly by a caller, and holds no
// ref count of its own.
template <class T> class future_state : public enable_shared_from_this<future_state<T>> {
public:
  using continuation_node = detail::continuation_node<T>;

  // What result_ actually stores on success: T itself, except when T is
  // void (variant can't hold void as an alternative), in which case a
  // stand-in tag type is used instead - see detail::void_value.
  using stored_t = std::conditional_t<std::is_void_v<T>, detail::void_value, T>;

  explicit future_state(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept
      : allocator_(allocator) {}

  future_state(const future_state&) = delete;
  auto operator=(const future_state&) -> future_state& = delete;
  future_state(future_state&&) = delete;
  auto operator=(future_state&&) -> future_state& = delete;

  // Destroys (without invoking) any continuation still queued: either the
  // promise was dropped without ever completing, or complete()'s drain
  // loop was aborted partway through by a throwing continuation (see
  // run()'s comment). Without this, those already-allocated nodes are
  // simply unreachable once this future_state itself is gone - a
  // permanent leak, not just a skipped notification.
  ~future_state() {
    while (auto* waiter = waiters_.dequeue()) {
      // Safe by construction, not by RTTI: every waiter ever enqueued
      // into waiters_ is a detail::waiter_node (set_continuation() only
      // accepts a continuation_node&, itself a waiter_node) - there is
      // no dynamic_cast alternative worth paying for here.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
      static_cast<detail::waiter_node*>(waiter)->destroy(allocator_);
    }
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
  // run once ready. If already ready, invokes it immediately instead of
  // queueing - single-threaded and synchronous for now; est::loop (M3)
  // will change this to schedule via the ready-queue instead.
  void set_continuation(continuation_node& node) {
    if (ready()) {
      run(node);
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
    return allocator_;
  }

  // Registers fn to run once ready. Two calling conventions, chosen by
  // how fn can be invoked (docs/PLAN.md, issue #23):
  //   - fn(future<T>&): "wrapped" - always called, whether this
  //     future_state succeeded or failed, with a fresh future<T> view of
  //     *this (built via shared_from_this() - future_state is a detail,
  //     not what a callback should see, see this class's own doc
  //     comment); fn inspects ready()/failed()/get() to decide what to
  //     do. No implicit unwrap.
  //   - fn(const T&), or fn() when T is void: "unwrapped" - called only
  //     on success, with the value itself (or no argument at all for
  //     void). On failure fn is *not* called; the returned future fails
  //     with the same exception instead. (Implemented by simply calling
  //     get() as fn's argument expression: get() rethrows on failure,
  //     which lands in the try/catch below exactly like an exception fn
  //     itself throws.)
  // Checked in that order, so a callback typed to take future<T>&
  // explicitly always gets the wrapped, no-unwrap behavior even if it
  // would incidentally also accept a T (e.g. a generic `auto&` lambda).
  //
  // The returned future's value type is fn's return type U, unless U is
  // itself a future<V> - futures are monadic, so a continuation that
  // returns a future is flattened into that future<V> directly rather
  // than producing a future<future<V>> a caller would have to unwrap
  // again themselves. Either way, an exception fn throws (or, in the
  // flattened case, that the inner future<V> fails with) is caught here
  // and routed into the returned future via set_exception() instead of
  // escaping - unlike this class's own set_value()/set_exception(),
  // which still abort complete()'s drain loop on an uncaught exception
  // (see run()'s comment), a chained continuation's failure is isolated
  // to its own downstream future and does not stop sibling continuations
  // from running. Runs synchronously, on whichever call stack completes
  // this future_state (M2 has no loop yet to defer onto - see
  // docs/PLAN.md, M3).
  template <detail::then_callback_for<T> Fn> auto then(Fn&& fn) {
    using decayed_fn = std::decay_t<Fn>;
    using downstream_value_type = detail::unwrap_future_t<raw_result_t<decayed_fn>>;
    auto downstream = shared_ptr<future_state<downstream_value_type>>::make(allocator_, allocator_);
    auto downstream_for_node = downstream; // copy: the node keeps its own reference too
    using node_type = concrete_continuation<decayed_fn, downstream_value_type>;
    auto* node = allocator_.template new_object<node_type>(std::forward<Fn>(fn),
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
    if constexpr (std::invocable<Fn&, future<T>&>) {
      return std::type_identity<std::invoke_result_t<Fn&, future<T>&>>{};
    } else if constexpr (std::is_void_v<T>) {
      return std::type_identity<std::invoke_result_t<Fn&>>{};
    } else {
      return std::type_identity<std::invoke_result_t<Fn&, const T&>>{};
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
        if constexpr (std::invocable<Fn&, future<T>&>) {
          // A fresh future<T> per invocation, not a stored one - this
          // continuation only runs once, so there's nothing to reuse it
          // for, and future_state<T> itself never keeps a future<T>
          // alive on its own account.
          future<T> view(state.shared_from_this());
          invoke_and_fulfill(view);
        } else if constexpr (std::is_void_v<T>) {
          state.get(); // rethrows on failure, caught below - fn_ is not called
          invoke_and_fulfill();
        } else {
          // state.get() rethrows on failure (caught below, fn_ not
          // called) before fn_ ever sees a value - the argument
          // expression is evaluated before invoke_and_fulfill() runs.
          invoke_and_fulfill(state.get());
        }
      } catch (...) {
        downstream_->set_exception(std::current_exception());
      }
    }

    // `this` here is concrete_continuation<Fn, U>*, so delete_object
    // deallocates with this type's actual size/alignment - the whole
    // reason destroy() is virtual instead of the caller deallocating
    // through a waiter_node& (see that class's own doc comment).
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
    // set_value() if it's a plain U, or - if it's itself a future<U> -
    // by registering a wrapped (no-unwrap) continuation on it that
    // forwards its eventual value/exception into downstream_ once it
    // resolves. That forwarding continuation reuses this same
    // get()-rethrows-into-catch propagation idiom as invoke() above,
    // rather than needing its own access to the inner future's private
    // state.
    template <class R> void fulfill(R&& result) {
      if constexpr (detail::is_future_v<std::decay_t<R>>) {
        using inner_value_type = detail::unwrap_future_t<std::decay_t<R>>;
        auto downstream_copy = downstream_; // copy: the forwarding lambda keeps its own reference
        std::forward<R>(result).then([downstream_copy](future<inner_value_type>& inner_future) {
          try {
            if constexpr (std::is_void_v<inner_value_type>) {
              inner_future.get();
              downstream_copy->set_value();
            } else {
              downstream_copy->set_value(inner_future.get());
            }
          } catch (...) {
            downstream_copy->set_exception(std::current_exception());
          }
        });
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

  // Drains every registered continuation (normally at most one - future
  // is meant to be a single-consumer handle - but nothing stops a caller
  // from registering more than one via then(), and the underlying list
  // already supports it; LIFO, same order est::waiter_list is documented
  // to use). Called by each setter after it has already stored the
  // result into result_ - this function only drains, it doesn't know or
  // care what was stored.
  void complete() {
    while (auto* waiter = waiters_.dequeue()) {
      run(*static_cast<continuation_node*>(waiter));
    }
  }

  // Guarantees the node is destroyed even if invoke() throws (a
  // continuation's own exception is not this future_state's problem to
  // swallow, but leaking the node it ran in would be a separate bug on
  // top of whatever the continuation did). A then()-created continuation
  // never actually throws out of invoke() - its own try/catch routes any
  // exception into its downstream future instead (see then()'s doc
  // comment) - but a continuation_node isn't required to go through
  // then(), so this guard stays: a hypothetical throwing one would still
  // abort the drain loop in complete() before any later-queued
  // continuations run, an accepted M2-scope limitation for that case, to
  // be revisited once M3's loop dispatches continuations independently
  // instead of inline on the completer's own call stack.
  void run(continuation_node& node) {
    scope_exit const guard{[&node, allocator = allocator_]() noexcept { node.destroy(allocator); }};
    node.invoke(*this);
  }

  waiter_list waiters_;
  std::variant<std::monostate, stored_t, std::exception_ptr> result_;
  std::pmr::polymorphic_allocator<std::byte> allocator_;
};

} // namespace est

export namespace est {

// Consumer handle: a thin, move-only view over a future_state<T>, backed
// by an est::shared_ptr so destroying a future does not destroy the
// future_state if something else - a still-live est::promise, or (once
// est::loop exists, M3) the loop's own keep-alive registration - still
// references it.
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
  shared_ptr<future_state<T>> state_;
};

} // namespace est
