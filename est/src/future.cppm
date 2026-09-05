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

// Forward declaration only: future_awaiter<T> (the type future<T>::operator
// co_await() below returns) needs future<T> itself complete - to call
// get() on it in await_resume() - so its own definition has to come after
// future<T>'s, further down this file. This forward declaration is enough
// for future<T> to name it in a friend declaration.
template <class T> class future_awaiter;

// A plain resumption trampoline: run() resumes whatever coroutine handle
// it was built with. Separately heap-allocated via an allocator (like
// every other ready_node this codebase queues - concrete_continuation<Fn,
// U>, concrete_timer_node<Fn>), *not* embedded inside the coroutine frame
// it resumes, for a subtle but real reason a first version of this class
// got wrong: an awaiter object embedded in a coroutine's own frame only
// lives for the duration of *its own* co_await expression - once run()
// resumes the coroutine past that point, the compiler is free to reuse
// that exact frame storage for whatever the coroutine's later code
// constructs (its next awaiter, a local variable, ...), since their
// lifetimes don't overlap. est::loop::run_one() (est:loop) calls
// destroy() on this same node *after* run() already returned - by then,
// for a frame-embedded node, that storage may already have been
// overwritten, and calling a virtual function through it is undefined
// behavior - confirmed the hard way (libc++abi: "Pure virtual function
// called!", a dispatch through a vtable pointer that had already been
// clobbered by the coroutine's own later frame activity) before landing
// on this heap-allocated design instead. A separately allocated node has
// its own real, independent lifetime, so run()-then-destroy() is exactly
// as safe here as it already is for every other ready_node in this
// codebase.
class coroutine_resume_node final : public ready_node {
public:
  explicit coroutine_resume_node(std::coroutine_handle<> handle) noexcept : handle_(handle) {}

  void run() final {
    ran_ = true;
    handle_.resume();
  }

  // If run() was never called - this node was still sitting in
  // est::loop's ready_ when the loop itself was destroyed, never
  // drained by a real drain_ready() pass - the coroutine is still fully
  // intact and untouched, so destroying it here is both safe and
  // necessary: nothing else will ever get a chance to free its frame
  // otherwise, a real, permanent leak (found in review, before this node
  // tracked ran_ at all). If run() *was* called, this node's job is done
  // either way, and touching handle_ again here would be wrong: the
  // coroutine either already self-destroyed via
  // promise_type::final_suspend()'s std::suspend_never (handle_ is now
  // dangling - calling anything on it is a use-after-free) or suspended
  // again on something else entirely, which now owns resuming (and
  // eventually destroying) it.
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept final {
    if (!ran_) {
      handle_.destroy();
    }
    allocator.delete_object(this);
  }

private:
  std::coroutine_handle<> handle_;
  bool ran_ = false;
};

// The "not yet started" half of a coroutine returning est::future<T> (see
// future<T>::promise_type below) - promise_type::initial_suspend()'s
// returned awaiter. Always suspends (await_ready() is unconditionally
// false) and, rather than ever running any of the coroutine body inline
// on the caller's own stack, hands a freshly heap-allocated
// coroutine_resume_node off to est::loop::enqueue_ready() to do that
// resumption later, during a drain pass - the same "never inline, always
// deferred through the loop" invariant every other producer in this
// codebase already keeps (future_state<T>::complete(), est::loop's own
// timer firing), so a caller of a coroutine-returning function can't
// tell, from calling-stack behavior alone, whether the est::future<T> it
// got back came from a coroutine or from a then() chain - the whole
// point of folding coroutine support directly into est::future<T>
// instead of a separate task<T> type.
//
// *this itself (unlike coroutine_resume_node above) *is* a coroutine-
// frame subobject - fine, since nothing ever reaches back into *this*
// after its own co_await expression (the one initial_suspend() itself
// is used for) concludes; await_suspend() below allocates the
// independently-lived node before that expression ends, and nothing
// downstream needs *this again afterward.
class coroutine_start_awaiter {
public:
  explicit coroutine_start_awaiter(loop& loop_ref) noexcept : loop_(loop_ref) {}
  // Deleted, not defaulted: never actually invoked - every use of this
  // class returns one as a genuine prvalue matching initial_suspend()'s
  // own return type, which C++17's mandatory copy elision builds
  // in place with no copy/move at all (the same guaranteed-elision
  // pattern est::scope_exit's own doc comment - est:util.scope_exit -
  // already documents relying on, needed here too since loop_ below is
  // a reference and so cannot be reassigned by an implicit copy/move
  // anyway).
  coroutine_start_awaiter(const coroutine_start_awaiter&) = delete;
  auto operator=(const coroutine_start_awaiter&) -> coroutine_start_awaiter& = delete;
  coroutine_start_awaiter(coroutine_start_awaiter&&) = delete;
  auto operator=(coroutine_start_awaiter&&) -> coroutine_start_awaiter& = delete;
  ~coroutine_start_awaiter() = default;

  // Not static, even though the body doesn't use *this: making it static
  // silences this one finding here, but every co_await call site using
  // this awaiter is compiler-generated code calling awaiter.await_ready()
  // through an instance regardless - trading one contained finding here
  // for a scattered "static member accessed through instance" complaint
  // at every call site instead, which is worse. NOLINT'd here, at the
  // single declaration, instead.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }

  void await_suspend(std::coroutine_handle<> handle) {
    auto* node = loop_.allocator().new_object<coroutine_resume_node>(handle);
    loop_.enqueue_ready(*node);
  }

  void await_resume() const noexcept {}

private:
  loop& loop_;
};

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
  constexpr std::size_t align = std::max(alignof(resource_ptr), alignof(std::max_align_t));
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
  constexpr std::size_t align = std::max(alignof(resource_ptr), alignof(std::max_align_t));
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
  // Requires the coroutine function's first parameter to be exactly
  // est::loop& - both this constructor and operator new below are
  // templated to match it (plus however many further parameters the
  // actual coroutine function takes) via the standard's "promise
  // constructor arguments"/allocator-argument matching, which tries
  // building promise_type from the coroutine call's own argument list
  // before ever falling back to a default constructor. There is
  // deliberately no default constructor here: a future<T>-returning
  // coroutine that doesn't take est::loop& first fails to compile with
  // "no matching constructor for promise_type" instead of silently
  // misbehaving.
  class promise_type : public detail::future_promise_result<T> {
  public:
    // Args&... /*unused*/: only loop_ref is ever read - the pack exists
    // purely so this constructor matches whatever further parameters the
    // actual coroutine function declares, per the "promise constructor
    // arguments" rule this whole design relies on (see this class's own
    // doc comment above).
    template <class... Args>
    explicit promise_type(loop& loop_ref, Args&... /*unused*/)
        : detail::future_promise_result<T>(
              shared_ptr<future_state<T>>::make(loop_ref.allocator(), loop_ref)),
          loop_(loop_ref) {}
    promise_type(const promise_type&) = delete;
    auto operator=(const promise_type&) -> promise_type& = delete;
    promise_type(promise_type&&) = delete;
    auto operator=(promise_type&&) -> promise_type& = delete;
    ~promise_type() = default;

    auto get_return_object() -> future { return future(this->state_); }

    // Always suspends, deferring the coroutine's first slice of work
    // onto est::loop's ready-queue instead of running any of it inline
    // on the calling stack - see detail::coroutine_start_awaiter's own
    // doc comment for why.
    auto initial_suspend() noexcept -> detail::coroutine_start_awaiter {
      return detail::coroutine_start_awaiter(loop_);
    }

    // Never suspends at the end: nothing outside this coroutine holds
    // (or needs) a coroutine_handle to it - only the future<T> returned
    // by get_return_object() (a shared_ptr<future_state<T>> underneath),
    // which by now already has its result via return_value()/
    // return_void()/unhandled_exception(). std::suspend_never here lets
    // the compiler destroy the coroutine frame immediately and
    // automatically once the body finishes. Safe to do so unconditionally
    // (unlike an earlier version of this design, which tried resuming
    // through a ready_node embedded *in* the frame being destroyed -
    // see coroutine_resume_node's own doc comment for why that didn't
    // work): every node that ever resumes this coroutine
    // (coroutine_resume_node, future_resume_node<T> below) is separately
    // heap-allocated, entirely independent of the frame this suspend
    // point destroys, so there is nothing left in this frame for anything
    // to touch afterward.
    auto final_suspend() noexcept -> std::suspend_never { return {}; }

    void unhandled_exception() { this->state_->set_exception(std::current_exception()); }

    // pmr-aware coroutine frame allocation - see
    // detail::coroutine_frame_alloc()/coroutine_frame_dealloc()'s own
    // doc comment.
    template <class... Args>
    static auto operator new(std::size_t size, loop& loop_ref, Args&... /*unused*/) -> void* {
      return detail::coroutine_frame_alloc(size, loop_ref.allocator());
    }

    static void operator delete(void* ptr, std::size_t size) noexcept {
      detail::coroutine_frame_dealloc(ptr, size);
    }

  private:
    loop& loop_;
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

// A plain resumption trampoline for a coroutine awaiting an est::future<T>
// - the T-parameterized twin of coroutine_resume_node above, needed here
// only because future_state<T>::set_continuation() requires exactly a
// detail::continuation_node<T>&, not the T-independent detail::ready_node
// coroutine_resume_node itself derives. invoke() ignores the
// future_state<T>& it's handed (this node doesn't need T for anything
// beyond satisfying that signature) and just resumes the handle, exactly
// like coroutine_resume_node::run() does. Separately heap-allocated for
// the identical reason - see that class's own doc comment.
template <class T> class future_resume_node final : public continuation_node<T> {
public:
  explicit future_resume_node(std::coroutine_handle<> handle) noexcept : handle_(handle) {}

  // future_state<T>& /*unused*/: continuation_node<T>::invoke() must take
  // one - this node doesn't need it for anything beyond satisfying that
  // signature.
  void invoke(future_state<T>& /*unused*/) override {
    invoked_ = true;
    handle_.resume();
  }

  // See coroutine_resume_node::destroy()'s own doc comment for the full
  // "why check invoked_ before touching handle_" reasoning - identical
  // here: if invoke() never ran (the future_state this node was
  // registered on was dropped without ever completing - docs/PLAN.md,
  // M2's abandoned-future design), the awaiting coroutine is still fully
  // intact and untouched, so this is the only chance to free its frame;
  // if invoke() did run, the coroutine either already self-destroyed or
  // suspended again on something else that now owns it, and touching
  // handle_ again here would be wrong either way.
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator) noexcept override {
    if (!invoked_) {
      handle_.destroy();
    }
    allocator.delete_object(this);
  }

private:
  std::coroutine_handle<> handle_;
  bool invoked_ = false;
};

// Awaiter for `co_await someFuture` on any est::future<T> - see
// future<T>::operator co_await() above. *this itself is a coroutine-frame
// subobject (like coroutine_start_awaiter above) - fine for the same
// reason: await_suspend() below hands the actual resumption off to a
// separately heap-allocated future_resume_node<T> before this awaiter's
// own co_await expression ends, and nothing reaches back into *this
// afterward.
template <class T> class future_awaiter {
public:
  explicit future_awaiter(future<T>& fut) noexcept : future_(fut) {}
  // Deleted, not defaulted - never actually invoked, same reasoning as
  // coroutine_start_awaiter's own identical deletions above (every use
  // returns a genuine prvalue future<T>::operator co_await() elides into
  // place).
  future_awaiter(const future_awaiter&) = delete;
  auto operator=(const future_awaiter&) -> future_awaiter& = delete;
  future_awaiter(future_awaiter&&) = delete;
  auto operator=(future_awaiter&&) -> future_awaiter& = delete;
  ~future_awaiter() = default;

  // Always suspends, even when future_.ready() is already true: matching
  // future_state<T>::then()'s own tested invariant that even an
  // already-ready registration defers through est::loop rather than
  // running inline (future_tests.cpp, "then() registered on an
  // already-ready future still defers to the loop"). A plain `return
  // future_.ready();` here would let co_await behave differently from
  // then() on the exact same already-ready future - running the rest of
  // the awaiting coroutine inline, on whatever call stack reached this
  // co_await, instead of yielding back to est::loop::drain_ready(). A
  // chain of several such co_awaits (each already ready - e.g. values set
  // directly via promise.set_value() rather than through the loop) would
  // then run to completion synchronously, starving any other work already
  // queued on that loop. future_state<T>::set_continuation() (called from
  // await_suspend() below) already handles the "already ready" case
  // correctly on its own - enqueueing straight onto est::loop's
  // ready-queue instead of invoking inline - so await_ready() doesn't
  // need to (and must not) special-case it here.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static) - see
  // detail::coroutine_start_awaiter::await_ready()'s own doc comment on
  // why not: every co_await call site would otherwise trade this one
  // finding for a readability-static-accessed-through-instance finding
  // there instead.
  [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }

  void await_suspend(std::coroutine_handle<> handle) {
    auto* node = future_.state_->allocator().template new_object<future_resume_node<T>>(handle);
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
