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
// the one thing that actually needs T (invoke()), plus the plumbing
// run() needs to call it once est::loop actually dequeues and runs this
// node. Not nested inside future_state<T> - it's an implementation
// detail of :future, not part of future_state's public surface, so it
// lives here instead.
template <class T> class continuation_node : public detail::ready_node {
public:
  void run() final { invoke(owner_); }

  // const shared_ptr<future_state<T>>&, not future_state<T>& - the one
  // place that actually needs an owning shared_ptr (concrete_continuation's
  // wrapped-mode branch, building a future<T> view to hand a callback) can
  // copy it out of the reference itself; everything else (future_resume_node,
  // flatten_forwarder, concrete_continuation's own unwrapped branch) never
  // pays for a refcount bump it doesn't need. Passing owner_ itself (a
  // reference to run()'s own member, no copy at the call site either)
  // is what replaced future_state<T>::shared_from_this() here - see
  // docs/wiki/Coroutines.md, "Why future<T> can't be copyable" and issue
  // #26.
  virtual void invoke(const shared_ptr<future_state<T>>& state) = 0;

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

  void invoke(const shared_ptr<future_state<T>>& state) override {
    if (state->failed()) {
      downstream_->set_exception(state->get_exception(), downstream_);
      return;
    }
    try {
      if constexpr (std::is_void_v<T>) {
        downstream_->set_value(downstream_);
      } else {
        downstream_->set_value(std::move(*state).get(), downstream_);
      }
    } catch (...) {
      downstream_->set_exception(std::current_exception(), downstream_);
    }
  }

  // bool /*ran*/ unused - see concrete_continuation<Fn, U>'s own doc
  // comment (further down this file) on why dropping downstream_
  // unconditionally here is the current behavior, not a settled answer.
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator,
               bool /*ran*/) noexcept override {
    allocator.delete_object(this);
  }

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
// allocator that built it, so the memory_resource* actually used has to
// be stashed somewhere operator delete can still find it - immediately
// past the frame itself, the pattern cppreference's own coroutine page
// documents for a stateful allocator ("Dynamic memory allocation for
// coroutine state").
[[nodiscard]] inline auto
coroutine_frame_alloc(std::size_t size, std::pmr::polymorphic_allocator<std::byte> allocator)
    -> void* {
  using resource_ptr = std::pmr::memory_resource*;
  // alignof(std::max_align_t), not std::max()'d (or std::min()'d) with
  // anything: the standard guarantees max_align_t's alignment is at least
  // as strict as every scalar type's, resource_ptr (a plain pointer)
  // included, so it alone already covers the trailing resource_ptr's
  // alignment. It also has to stand in for the coroutine frame's own
  // alignment requirement, which this function has no way to query
  // directly (the compiler passes only size, not alignment, to a custom
  // promise_type::operator new) - std::min(alignof(resource_ptr), ...)
  // would silently under-align the frame itself relative to whatever the
  // compiler assumes, undefined behavior for any frame needing more than
  // pointer alignment (which is the common case).
  constexpr std::size_t align = alignof(std::max_align_t);
  const std::size_t padded_size = (size + align - 1) / align * align;
  auto* const resource = allocator.resource();
  void* const frame = resource->allocate(padded_size + sizeof(resource_ptr), align);
  // Placement-news the resource pointer into the padding just past the
  // frame - the pointer arithmetic below is exactly the documented
  // layout this pair of functions establishes, not an out-of-bounds risk
  // (padded_size + sizeof(resource_ptr) bytes were just allocated for
  // precisely this).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  ::new (static_cast<std::byte*>(frame) + padded_size) resource_ptr(resource);
  return frame;
}

inline void coroutine_frame_dealloc(void* frame, std::size_t size) noexcept {
  using resource_ptr = std::pmr::memory_resource*;
  // Must recompute the identical align/padded_size coroutine_frame_alloc()
  // used - see that function's own comment on why this is
  // alignof(std::max_align_t) alone, not std::max()'d or std::min()'d with
  // anything.
  constexpr std::size_t align = alignof(std::max_align_t);
  const std::size_t padded_size = (size + align - 1) / align * align;
  // Reads back the memory_resource* coroutine_frame_alloc() stashed just
  // past the frame - safe by construction, not by RTTI (the
  // reinterpret_cast) or out of bounds (the pointer arithmetic): every
  // frame ever handed back by coroutine_frame_alloc() has one there, at
  // exactly this offset.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  auto* const resource_addr =
      reinterpret_cast<resource_ptr*>(static_cast<std::byte*>(frame) + padded_size);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::pmr::memory_resource* const resource = *resource_addr;
  resource->deallocate(frame, padded_size + sizeof(resource_ptr), align);
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
// built from a continuation_node<T>'s own owner_ reference (below),
// rather than a future_state<T>&.
// Holds value-or-exception storage and a continuation slot built on
// est::intrusive_list. Lifetime is managed externally by an
// est::shared_ptr<future_state<T>> (see make_promise_future() in
// est:promise) - never constructed directly by a caller, and holds no
// ref count of its own.
//
// Does not inherit est::enable_shared_from_this<future_state<T>> (issue
// #26; it did until then, back when set_continuation()/complete() and
// concrete_continuation's wrapped-mode view each computed their own
// shared_ptr<future_state<T>> via shared_from_this()). Every one of
// those call sites is reached from external code that already holds a
// shared_ptr<future_state<T>> pointing at the exact same object - a
// caller's own promise<T>/future<T>::state_, or a continuation_node's
// own owner_ - so set_value()/set_exception()/set_continuation()/then()
// now take that shared_ptr explicitly (conventionally named `self`)
// instead of manufacturing a fresh one internally.
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
template <class T> class future_state {
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
    waiters_.drain([this](continuation_node& node) { node.destroy(loop_.allocator(), false); });
  }

  // `self` is a shared_ptr aliasing *this, supplied by a caller that
  // already holds one (promise<T>::state_, or a coroutine promise_type's
  // own state_) - see this class's own doc comment on why it's an
  // explicit parameter now rather than computed internally via
  // shared_from_this(). Forwarded straight into complete(), unused
  // otherwise.
  void set_value(const shared_ptr<future_state>& self)
    requires std::is_void_v<T>
  {
    check_not_completed();
    result_.template emplace<stored_t>();
    complete(self);
  }

  // stored_t, not T, in these two signatures - a requires-clause only
  // gates overload resolution, it doesn't stop const T&/T&& from being
  // elaborated (and, for T=void, rejected as "reference to void") the
  // moment future_state<void> itself is instantiated. stored_t is never
  // actually void, so it sidesteps the problem entirely; it's also
  // simply T whenever T isn't void, so this changes nothing observable
  // for every T this class was already used with.
  void set_value(const stored_t& value, const shared_ptr<future_state>& self)
    requires(!std::is_void_v<T>)
  {
    check_not_completed();
    result_.template emplace<stored_t>(value);
    complete(self);
  }

  void set_value(stored_t&& value, const shared_ptr<future_state>& self)
    requires(!std::is_void_v<T>)
  {
    check_not_completed();
    result_.template emplace<stored_t>(std::move(value));
    complete(self);
  }

  void set_exception(std::exception_ptr&& exception, const shared_ptr<future_state>& self) {
    check_not_completed();
    result_.template emplace<std::exception_ptr>(std::move(exception));
    complete(self);
  }

  // Registers a continuation node (already allocated via allocator()) to
  // run once ready. If already ready, hands it straight to est::loop's
  // ready-queue instead of queueing it locally - either way, this
  // future_state never invokes a continuation itself; est::loop always
  // does, on its own drain pass, never inline on this call stack (M3,
  // docs/PLAN.md). `self`: see this class's own doc comment - bound to
  // the node (a real, owning reference this future_state itself can no
  // longer manufacture) only on this ready-now path; the not-yet-ready
  // path below doesn't need it at all yet, since bind_owner() only
  // happens later, from complete().
  void set_continuation(continuation_node& node, const shared_ptr<future_state>& self) {
    if (ready()) {
      node.bind_owner(self);
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
  //     *this (built from the continuation node's own owner_ reference -
  //     future_state is a detail, not what a callback should see, see
  //     this class's own doc comment); fn inspects ready()/failed()/get()
  //     to decide what to do. No implicit unwrap.
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
  // `self`: see this class's own doc comment - forwarded straight into
  // set_continuation(), unused otherwise.
  template <detail::then_callback_for<T> Fn>
  auto then(Fn&& fn, const shared_ptr<future_state>& self) {
    using decayed_fn = std::decay_t<Fn>;
    using downstream_value_type = detail::unwrap_future_t<raw_result_t<decayed_fn>>;
    auto downstream =
        shared_ptr<future_state<downstream_value_type>>::make(loop_.allocator(), loop_);
    auto downstream_for_node = downstream; // copy: the node keeps its own reference too
    using node_type = concrete_continuation<decayed_fn, downstream_value_type>;
    auto* node = loop_.allocator().template new_object<node_type>(std::forward<Fn>(fn),
                                                                  std::move(downstream_for_node));
    set_continuation(*node, self);
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

    void invoke(const shared_ptr<future_state>& state) override {
      try {
        if constexpr (detail::invocable_unwrapped<Fn, T>()) {
          if (state->failed()) {
            // Unwrapped mode's auto-propagate-on-failure, fn_ not called:
            // failed() already tells us there's an exception waiting, so
            // fetching it via get_exception() is a plain pointer copy -
            // cheaper than the alternative of calling get() purely to
            // have it rethrow into the catch below, even though this
            // isn't the happy path either way.
            downstream_->set_exception(state->get_exception(), downstream_);
          } else if constexpr (std::is_void_v<T>) {
            invoke_and_fulfill();
          } else {
            invoke_and_fulfill(state->get());
          }
        } else {
          // Wrapped mode: unwrapped isn't viable for Fn (checked first,
          // above - see then()'s own doc comment on precedence, issue
          // #39), so then_callback_for<Fn, T> guarantees Fn is invocable
          // with future<T>& instead. A fresh future<T> per invocation,
          // not a stored one - this continuation only runs once, so
          // there's nothing to reuse it for, and future_state<T> itself
          // never keeps a future<T> alive on its own account. Copies
          // `state` (this override's own const& parameter, ultimately
          // aliasing the node's own owner_) into future<T>'s by-value
          // constructor - the one refcount bump this whole call pays,
          // only on this branch (issue #26: no shared_from_this() left
          // anywhere in this file).
          future<T> view(state);
          invoke_and_fulfill(view);
        }
      } catch (...) {
        downstream_->set_exception(std::current_exception(), downstream_);
      }
    }

    // `this` here is concrete_continuation<Fn, U>*, so delete_object
    // deallocates with this type's actual size/alignment - the whole
    // reason destroy() is virtual instead of the caller deallocating
    // through a detail::ready_node& (see that class's own doc comment,
    // est:loop). bool /*ran*/ unused here: downstream_ is currently just
    // dropped on abandonment either way, unlike est::mutex's resume
    // nodes or future_resume_node<T> below - whether a coroutine
    // co_await-ing a `.then()`-chained future can be stranded the same
    // way if the *upstream* future_state is dropped first is a real,
    // separate question this class doesn't yet answer either way (not
    // addressed here - see issue #50's discussion for the identical
    // shape of hazard in sleep_for()/sleep_until()).
    void destroy(std::pmr::polymorphic_allocator<std::byte> allocator,
                 bool /*ran*/) noexcept override {
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
        downstream_->set_value(downstream_);
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
        result.state_->set_continuation(*node, result.state_);
      } else {
        downstream_->set_value(std::forward<R>(result), downstream_);
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
  // binds its own copy of `self` (see this class's own doc comment)
  // right before being handed off (see continuation_node<T>::
  // bind_owner()'s own doc comment on why not earlier), so this
  // future_state is guaranteed to survive until est::loop actually runs
  // (and destroys) it, even if every other reference to it (promise,
  // future) is dropped in the meantime.
  void complete(const shared_ptr<future_state>& self) {
    waiters_.drain([&self, this](continuation_node& node) {
      node.bind_owner(self);
      loop_.enqueue_ready(node);
    });
  }

  loop& loop_;
  intrusive_list<continuation_node> waiters_;
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
  void return_value(const T& value) { state_->set_value(value, state_); }
  void return_value(T&& value) { state_->set_value(std::move(value), state_); }

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
  void return_void() { state_->set_value(state_); }

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

  // Returns another future<T> aliasing the same future_state as *this -
  // both see the same eventual result, and either may safely call the
  // lvalue (copying) get() or register any number of then() callbacks,
  // any number of times. What it does *not* make safe: the consuming
  // (rvalue) get() path - which co_await always uses (this class's own
  // operator co_await(), below) - called from more than one clone of a
  // value-carrying future<T>. The second such call reads the first's
  // moved-from leftovers, silently; see docs/wiki/Coroutines.md, "Why
  // future<T> can't be copyable," for the full reasoning. Safe
  // unconditionally only when T is void (nothing to consume) or every
  // clone sticks to the non-consuming paths. Issue #26.
  [[nodiscard]] auto clone() const -> future { return future(state_); }

  // Forwards to future_state<T>::then() (see its own doc comment) - the
  // node allocation and registration live there now, not here.
  template <class Fn> auto then(Fn&& fn) { return state_->then(std::forward<Fn>(fn), state_); }

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
  // est::future<T> - `est::future<int> foo(est::loop& loop_ref, ...) {
  // co_await ...; co_return 42; }`. Deliberately not a separate task<T>
  // wrapper type: the whole point is that a caller of foo() gets back a
  // plain est::future<int>, indistinguishable from one built out of
  // sleep_for()/then() chains - nothing about future<T>'s own interface
  // changes; this class only exists for the compiler's coroutine
  // machinery to find via the standard promise_type protocol.
  //
  // A coroutine function may (still the original M4 convention) take
  // est::loop& as its first parameter - both this constructor and
  // operator new below are templated to match it (plus however many
  // further parameters the actual coroutine function takes) via the
  // standard's "promise constructor arguments"/allocator-argument
  // matching, which tries building promise_type from the coroutine
  // call's own argument list before ever falling back to a default
  // constructor. Or (issue #30) it may take no loop& at all - a second
  // constructor/operator new pair below falls back to
  // est::current_loop() (est:util.current_loop) instead, for a caller
  // content relying on whichever loop is current rather than threading
  // one through by hand. Or it may take no parameters
  // whatsoever - a third, non-template pair covers that case, since a
  // bare parameter pack can't match zero arguments against "at least one
  // parameter."
  class promise_type : public detail::future_promise_result<T> {
  public:
    // Args&... /*unused*/: only loop_ref is ever read - the pack exists
    // purely so this constructor matches whatever further parameters the
    // actual coroutine function declares, per the "promise constructor
    // arguments" rule this whole design relies on (see this class's own
    // doc comment above). loop_ref itself isn't stored - it's only ever
    // needed here, to build state_ - now that initial_suspend() no longer
    // needs a loop& of its own to build a coroutine_start_awaiter from
    // (PR #37 review follow-up), nothing in this class touches it again
    // after construction.
    template <class... Args>
    explicit promise_type(loop& loop_ref, Args&... /*unused*/)
        : detail::future_promise_result<T>(
              shared_ptr<future_state<T>>::make(loop_ref.allocator(), loop_ref)) {}

    // Issue #30: a coroutine whose own first parameter isn't est::loop&
    // falls back to est::current_loop() instead. Constrained to exclude
    // a leading loop& specifically (std::same_as, not a broader
    // "convertible to" - matching the exact-type match the constructor
    // above already relies on) so this never competes with it for a call
    // that *does* pass one: without the constraint, both constructors
    // would deduce to the identical actual parameter list for such a
    // call ((loop&, Rest&...) either way, since a bare parameter pack
    // happily absorbs a leading loop& into Rest itself) - an ambiguity
    // conversion ranking alone can't break, since the two candidates
    // would be indistinguishable by it.
    //
    // Delegates to the constructor above (Args deduced empty) rather than
    // repeating its body - per review, all three constructors should
    // share the one place that actually builds state_. current_loop()'s
    // own precondition (a loop must actually be current) is checked
    // exactly once either way.
    template <class First, class... Rest>
      requires(!std::same_as<std::remove_cvref_t<First>, loop>)
    explicit promise_type(First& /*unused*/, Rest&... /*unused*/) : promise_type(current_loop()) {}

    // Issue #30: a coroutine taking no parameters at all - the
    // constructor above needs at least one (First is not optional), so
    // this needs its own, non-template overload. Delegates the same way.
    promise_type() : promise_type(current_loop()) {}

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
    // on the other end of a co_await, and PR #37's own review discussion
    // for why this replaced an earlier, always-deferred design (a
    // coroutine_start_awaiter that unconditionally suspended into
    // est::loop's ready-queue before running anything, at the cost of one
    // extra heap-allocated resume node and ready-queue round trip per
    // coroutine call, even for one that never awaits anything at all).
    auto initial_suspend() noexcept -> std::suspend_never { return {}; }

    // Never suspends at the end either: nothing outside this coroutine
    // holds (or needs) a coroutine_handle to it - only the future<T>
    // returned by get_return_object() (a shared_ptr<future_state<T>>
    // underneath), which by now already has its result via
    // return_value()/return_void()/unhandled_exception(). std::suspend_never
    // here lets the compiler destroy the coroutine frame immediately and
    // automatically once the body finishes. Safe to do so unconditionally
    // (unlike an earlier version of this design, which tried resuming
    // through a ready_node embedded *in* the frame being destroyed - see
    // future_resume_node<T>'s own doc comment for why that didn't work):
    // every node that ever resumes this coroutine (future_resume_node<T>
    // below, mutex::lock_resume_node/acquire_resume_node) is separately
    // heap-allocated, entirely independent of the frame this suspend
    // point destroys, so there is nothing left in this frame for anything
    // to touch afterward.
    auto final_suspend() noexcept -> std::suspend_never { return {}; }

    void unhandled_exception() {
      this->state_->set_exception(std::current_exception(), this->state_);
    }

    // pmr-aware coroutine frame allocation - see
    // detail::coroutine_frame_alloc()/coroutine_frame_dealloc()'s own
    // doc comment. Three overloads mirroring the three constructors
    // above, matched against the exact same argument list by the same
    // "promise constructor arguments" rule - the compiler picks whichever
    // operator new and whichever constructor line up with the coroutine
    // call's own arguments together, so these always agree on which loop
    // to use.
    template <class... Args>
    static auto operator new(std::size_t size, loop& loop_ref, Args&... /*unused*/) -> void* {
      return detail::coroutine_frame_alloc(size, loop_ref.allocator());
    }

    template <class First, class... Rest>
      requires(!std::same_as<std::remove_cvref_t<First>, loop>)
    static auto operator new(std::size_t size, First& /*unused*/, Rest&... /*unused*/) -> void* {
      return detail::coroutine_frame_alloc(size, current_loop().allocator());
    }

    static auto operator new(std::size_t size) -> void* {
      return detail::coroutine_frame_alloc(size, current_loop().allocator());
    }

    static void operator delete(void* ptr, std::size_t size) noexcept {
      detail::coroutine_frame_dealloc(ptr, size);
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
// est:promise's sleep_resume_node/yield_resume_node), *not* embedded
// inside the coroutine frame it
// resumes, for a subtle but real reason a first version of this class got
// wrong: an awaiter object embedded in a coroutine's own frame only lives
// for the duration of *its own* co_await expression - once run() resumes
// the coroutine past that point, the compiler is free to reuse that exact
// frame storage for whatever the coroutine's later code constructs (its
// next awaiter, a local variable, ...), since their lifetimes don't
// overlap. est::loop::run_one() (est:loop) calls destroy() on this same
// node *after* run() already returned - by then, for a frame-embedded
// node, that storage may already have been overwritten, and calling a
// virtual function through it is undefined behavior - confirmed the hard
// way (libc++abi: "Pure virtual function called!", a dispatch through a
// vtable pointer that had already been clobbered by the coroutine's own
// later frame activity) before landing on this heap-allocated design
// instead. A separately allocated node has its own real, independent
// lifetime, so run()-then-destroy() is exactly as safe here as it already
// is for every other ready_node in this codebase.
template <class T> class future_resume_node final : public continuation_node<T> {
public:
  explicit future_resume_node(std::coroutine_handle<> handle) noexcept : handle_(handle) {}

  // const shared_ptr<future_state<T>>& /*unused*/: continuation_node<T>::
  // invoke() must take one - this node doesn't need it for anything
  // beyond satisfying that signature.
  void invoke(const shared_ptr<future_state<T>>& /*unused*/) override { handle_.resume(); }

  // If invoke() never ran (the future_state this node was registered on
  // was dropped without ever completing - docs/PLAN.md, M2's
  // abandoned-future design - so `ran` is false, per ready_node::
  // destroy()'s own doc comment, est:loop), the awaiting coroutine is
  // still fully intact and untouched, so this is the only chance to free
  // its frame; if invoke() did run, the coroutine either already
  // self-destroyed or suspended again on something else that now owns
  // it, and touching handle_ again here would be wrong either way.
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept override {
    if (!ran) {
      handle_.destroy();
    }
    allocator.delete_object(this);
  }

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
  // waited for" reasoning promise_type::initial_suspend() (above) now
  // uses at the other end of a coroutine's lifetime (PR #37's own review
  // discussion). An earlier version of this unconditionally returned
  // `false`, deliberately matching future_state<T>::then()'s own "never
  // run inline, even when already ready" invariant - found, at the time,
  // to be the more defensible default (a caller of a coroutine-returning
  // function couldn't otherwise assume a `co_await` never ran some of the
  // awaited producer's own logic inline on an unexpected call stack).
  // That tradeoff was revisited once initial_suspend() itself adopted the
  // same "skip a suspend that isn't needed" stance: paying for a
  // future_resume_node<T> allocation and a full ready-queue round trip
  // purely to resume something that was never actually going to wait for
  // anything stopped being worth it, for the identical reason it stopped
  // being worth it there. future_state<T>::set_continuation() (called
  // from await_suspend() below) still handles the "not yet ready" case
  // correctly either way - this only changes whether that call, and the
  // node it needs, happens at all.
  [[nodiscard]] auto await_ready() const noexcept -> bool { return future_.ready(); }

  void await_suspend(std::coroutine_handle<> handle) {
    auto* node = future_.state_->allocator().template new_object<future_resume_node<T>>(handle);
    future_.state_->set_continuation(*node, future_.state_);
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
