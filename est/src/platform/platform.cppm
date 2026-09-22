export module est:platform;

import std;
import :util.scope_exit;

// The runtime-polymorphic seam est::check()/est::timer_queue actually
// need from "the platform" - a monotonic clock, and an answer to "what
// happens when a check fails." `interface` is deliberately the *only*
// thing this module knows about a platform: a pure abstract base with no
// concrete backend of its own. The one concrete backend, `hosted_stdcpp`,
// lives in `estext`, a wholly separate module (`estext/src/
// hosted_stdcpp.cppm`).
//
// This is virtual dispatch through a `thread_local` object, not a
// compile-time template parameter - est::timer_queue has no Platform
// template parameter, at the cost of one indirect call per uptime()/
// assert_failure() instead of a direct one. `thread_local` (not a single
// process-global) so a multi-core, no-MMU target can give each core its
// own installed backend independently, without any of them synchronizing
// on shared mutable state to do it - see override_instance()'s own doc
// comment. A future bare-metal backend is a second `final` class
// implementing `interface`, in its own module, installed the same way -
// not a framework redesign either way.
//
// est::loop's own "current loop" registration used to live behind this
// interface too (get_current_loop_context()/set_current_loop_context(),
// removed) - `:util.current_loop` now owns that directly via its own
// `thread_local` storage instead, needing nothing from `:platform` at
// all. That also removes the one reason this module used to need an
// exported forward declaration of `est::loop`: nothing here names it any
// more.

export namespace est::platform {

// A framework-owned vocabulary clock, deliberately not
// std::chrono::steady_clock itself: no variant of the ARM bare-metal
// toolchain estpico builds against (arm-none-eabi's own prebuilt libc++)
// provides it at all - every one is built with
// _LIBCPP_HAS_NO_MONOTONIC_CLOCK, which removes std::chrono::steady_clock's
// class declaration entirely, not just its implementation, so there's no
// way to make it exist by supplying a working clock underneath. Same
// shape as std::chrono::steady_clock (nanosecond resolution, marked
// steady) so every existing use of steady_clock::time_point/duration
// throughout est's own source (loop.cppm/timer.cppm/jitter.cppm) becomes
// a type-tag swap rather than a semantic rewrite - the underlying
// representation (a signed 64-bit nanosecond count) is unchanged.
//
// No now() static member: unlike std::chrono::steady_clock, nothing in
// est ever calls clock::now() directly - every actual "what time is it"
// question already goes through interface::uptime() below (virtual
// dispatch to whichever backend is installed), and each backend's own
// uptime() override is free to source the value however it likes (real
// std::chrono::steady_clock::now() for hosted_stdcpp, a JS import for
// platform_wasm, a hardware timer for estpico) - clock itself is purely a
// vocabulary type identifying "the time_point/duration platform::interface
// speaks in," not a working clock any code calls into on its own. The name
// deliberately isn't now(): every implementation of this method returns a
// value relative to an arbitrary, backend-chosen epoch (this process/
// board's own start, not wall-clock "the current time") - the same
// contract std::chrono::steady_clock::now() itself has, just spelled to
// say so instead of inviting the wall-clock reading "now()" suggests.
struct clock {
  using duration = std::chrono::nanoseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<clock, duration>;
  static constexpr bool is_steady = true;
};

class interface;

// Declared here (defined later, once detail::current_instance exists for
// its body to reference) purely so printdbg() below - and, transitively,
// hosted_stdcpp::detect_loop_stall() (estext) - can call it. See
// instance()'s own canonical doc comment further down for what it
// actually does.
[[nodiscard]] auto instance() noexcept -> interface&;

// A best-effort, nothrow debug diagnostic. Does the compile-time-checked
// formatting itself (so a caller gets std::format_string<Ts...>'s usual
// call-site diagnostics for a mismatched format string) and hands the
// type-erased result to interface::vprintdbg() below - mirroring
// std::print()'s own split from std::vprint_unicode(). Declared ahead of
// interface (rather than in its usual spot further down, next to
// override_instance()) specifically so detect_loop_stall() can call it -
// defined only after interface is a complete type, further down (calling
// vprintdbg() on the reference instance() returns needs a complete type,
// not just this forward declaration).
//
// Backend-swappable: a thin wrapper deferring to whichever
// platform::interface is currently installed. Only the formatting step
// has to be a template (std::format_string<Ts...>'s compile-time check
// needs the caller's own argument types); the actual "where does this
// text go" decision belongs to vprintdbg(), which isn't a template.
template <class... Ts> void printdbg(std::format_string<Ts...> fmt, Ts&&... args) noexcept;

// A pure interface, deliberately: every method below is pure virtual and
// this class holds no data members of its own - "the implementation"
// (state included) belongs entirely to whichever concrete backend needs
// it (hosted_stdcpp, module estext), not to this abstraction. A future
// bare-metal backend implements every method itself, the same as
// hosted_stdcpp does.
class interface {
public:
  interface() = default;
  interface(const interface&) = delete;
  auto operator=(const interface&) -> interface& = delete;
  interface(interface&&) = delete;
  auto operator=(interface&&) -> interface& = delete;
  virtual ~interface() = default;

  [[nodiscard]] virtual auto uptime() const noexcept -> clock::time_point = 0;

  // Blocks the calling thread until `deadline`, or returns immediately if
  // it has already passed - est::loop's answer to "how do I wait for the
  // next timer" without a busy-loop, same reasoning as uptime()/
  // assert_failure() being platform hooks: a test fake overrides this to
  // advance its own fake clock instead of actually blocking, so a loop
  // test exercising real timer-driven wakeups runs instantly instead of
  // for real wall-clock seconds (est/tests/loop_tests.cpp).
  virtual void sleep_until(clock::time_point deadline) const noexcept = 0;

  // Returns a fresh, best-effort-random seed value for anything in est
  // that needs one (est::jitter, :util.jitter, the only caller today) -
  // "where does randomness come from" is a platform decision the same way
  // uptime()/assert_failure() are: hosted_stdcpp answers via
  // std::random_device (estext), a future bare-metal backend from
  // whatever hardware entropy source it has. Not required to be
  // cryptographically secure, or even high-quality - jitter only needs to
  // differ from the last draw, never to resist prediction.
  [[nodiscard]] virtual auto get_random_seed() const noexcept -> std::uint64_t = 0;

  // Reports a failed est::check() and terminates - the platform's answer
  // to "what actually happens when a check fails," same reasoning as
  // uptime() being the answer to "how long have I been running": a
  // bare-metal backend,
  // or a test fake, gets to answer this differently (halt, trigger a
  // debug break, ...) without est::check() itself changing.
  [[noreturn]] virtual void assert_failure(std::string_view message,
                                           std::source_location location) const noexcept = 0;

  // Emits a formatted debug diagnostic - the "where does this text
  // actually go" half of printdbg() above (see its own doc comment),
  // mirroring std::vprint_unicode()'s split from std::print(): printdbg()
  // does the compile-time-checked formatting and type-erases the
  // arguments via std::make_format_args(), this decides what happens to
  // the result. An implementation must swallow its own failures (an I/O
  // error, e.g.) internally - printdbg() itself already promises nothrow,
  // best-effort behavior, and can't do that on this method's behalf
  // without seeing inside it.
  virtual void vprintdbg(std::string_view fmt, std::format_args args) const noexcept = 0;

  // Marks the start of a fresh "how long does the next node/timer
  // callback take" measurement window - est::loop::run_one() calls this
  // immediately before running one, and detect_loop_stall() below
  // immediately after. Moved onto platform for the same reason uptime()/
  // sleep_until() are platform hooks rather than est::loop calling
  // std::chrono/std::this_thread directly: "how do we know a callback
  // ran long" is a policy a backend should get to answer for itself.
  // hosted_stdcpp's own override just records uptime() into a member for
  // its detect_loop_stall() to compare against - the only strategy that
  // makes sense for a single-threaded, synchronous-checkpoint backend. A
  // future backend could run a watchdog on a background thread instead.
  virtual void reset_loop_stall_detection() noexcept = 0;

  // Checked by est::loop::run_one() right after a node/timer callback
  // returns: has it been longer than `threshold` since the matching
  // reset_loop_stall_detection() call? If so, reports a best-effort
  // diagnostic via printdbg() above - single-threaded means one slow
  // continuation blocks everything else a loop owns, with nothing to
  // preempt it, so a runaway handler should at least show up as a clear
  // diagnostic instead of "the whole program mysteriously stalled."
  // `threshold` is passed in here rather than baked into
  // reset_loop_stall_detection(), so est::loop's own
  // long_running_threshold stays the single source of truth for the
  // value, unchanged by which backend is installed.
  virtual void detect_loop_stall(clock::duration threshold) const noexcept = 0;
};

// The actual body, deferred until here (see the forward declaration's own
// doc comment above): calling vprintdbg() on the reference instance()
// returns needs interface to be a complete type, which it only just
// became.
//
// args deliberately isn't std::forward'd below: std::make_format_args()
// itself takes its arguments by plain lvalue reference (Args&..., not a
// forwarding reference), storing references into the format_args it
// returns for vprintdbg() to use immediately afterward - forwarding an
// rvalue argument here would bind that reference to a temporary about to
// expire, not extend anything's lifetime the way it might look like it
// should.
// __cpp_exceptions gate (est/src/future.cppm's own concrete_continuation<
// Fn, U>::run() comment has the full reasoning): instance().vprintdbg()
// genuinely can throw when exceptions are enabled, but -fno-exceptions
// makes `throw` illegal everywhere in the TU, so nothing reaches this
// catch on such a build and the compiler won't let this function spell
// it.
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
template <class... Ts> void printdbg(std::format_string<Ts...> fmt, Ts&&... args) noexcept {
#ifdef __cpp_exceptions
  try {
    instance().vprintdbg(fmt.get(), std::make_format_args(args...));
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (...) {
  }
#else
  instance().vprintdbg(fmt.get(), std::make_format_args(args...));
#endif
}

} // namespace est::platform

// Not part of est::platform's exported surface (unlike future.cppm's own
// est::detail, this one is nested under est::platform specifically since
// it's platform-local state, not a framework-wide implementation detail)
// - a plain, non-exported `namespace est::platform::detail` here, so
// `current_instance` stays reachable only from within this module, not
// assignable by any `import est;` consumer bypassing instance()/
// override_instance() below.
namespace est::platform::detail {
// No default backend constructed here: the only backend that exists,
// hosted_stdcpp, lives in its own module (estext), which :platform can't
// import without inverting the dependency DAG. Starts null on every
// thread/core; each one's own entry point installs a concrete interface
// via override_instance() before running any code that calls instance() -
// see examples/hello_world/main.cpp or est/tests/test_main.cpp.
//
// thread_local, not a single process-global: a plain global would need
// every core on a shared-memory, no-MMU multicore target to agree on (and
// serialize writes to) one slot, even though each core only ever installs
// its own backend once and never touches another core's. thread_local
// gives each core - or, on a hosted OS, each thread - an independent slot
// for the price of one relative-addressed load, no synchronization
// needed, the same reasoning current_loop() (est:util.current_loop)
// applies to its own storage.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline thread_local interface* current_instance = nullptr;
} // namespace est::platform::detail

export namespace est::platform {

// The active platform object est::check()/est::timer_queue actually call
// through, for whichever thread/core calls instance(). Nothing is
// installed until that thread/core's own entry point calls
// override_instance() with a concrete backend (typically
// estext::hosted_stdcpp) - see detail::current_instance's own doc
// comment.
[[nodiscard]] inline auto instance() noexcept -> interface& {
  return *detail::current_instance;
}

// Points instance() at `replacement`, for the calling thread/core only,
// until the returned guard is destroyed, restoring whatever was current
// before - nests correctly, since each returned guard only remembers what
// it personally replaced. Built on est::scope_exit rather than a
// hand-rolled RAII type: the swap-then-restore shape is exactly what
// scope_exit already exists for (see its own doc comment).
//
// Mutates only this thread's/core's own thread_local slot, not shared
// state another one could be reading concurrently - safe to call from
// every core of a shared-memory target independently and concurrently,
// each installing its own backend, with nothing to synchronize.
[[nodiscard]] inline auto override_instance(interface& replacement) noexcept {
  interface* const previous = std::exchange(detail::current_instance, &replacement);
  return scope_exit([previous]() noexcept { detail::current_instance = previous; });
}

} // namespace est::platform
