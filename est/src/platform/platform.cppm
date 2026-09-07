export module est:platform;

import std;
import :util.scope_exit;

// The hosted-stdcpp platform backend, and the runtime-polymorphic seam it
// plugs into. `interface` is what est::check()/est::timer_queue actually
// need from "the platform" - a monotonic clock, and an answer to "what
// happens when a check fails." `hosted_stdcpp` is the only implementation
// that exists so far.
//
// This is virtual dispatch through a single global object, not a
// compile-time template parameter (docs/PLAN.md records why: a prior,
// narrower proposal - decl/def split with no change to the templating -
// was rejected; this design actually drops est::timer_queue's Platform
// template parameter, at the cost of one indirect call per now()/
// assert_failure() instead of a direct one). A future bare-metal backend
// is a second `final` class implementing `interface`, installed as the
// global instance at startup, not a framework redesign.
export namespace est::platform {

class interface;

// Declared here (defined later, once hosted_stdcpp/detail::
// current_instance exist for its body to reference) purely so printdbg()
// below - and, transitively, hosted_stdcpp::detect_loop_stall() - can
// call it. See instance()'s own canonical doc comment further down for
// what it actually does.
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
// Unlike an earlier version, this *is* backend-swappable - it's now a
// thin wrapper deferring to whichever platform::interface is currently
// installed, per review: "printdbg should defer to the interface." Only
// the formatting step has to be a template (std::format_string<Ts...>'s
// compile-time check needs the caller's own argument types); the actual
// "where does this text go" decision now belongs to vprintdbg(), which
// isn't.
template <class... Ts> void printdbg(std::format_string<Ts...> fmt, Ts&&... args) noexcept;

// A pure interface, deliberately: every method below is pure virtual and
// this class holds no data members of its own - per review, "the
// implementation" (state included) belongs entirely to whichever concrete
// backend needs it (hosted_stdcpp, below), not to this abstraction. Two
// methods here (reset_loop_stall_detection()/detect_loop_stall(),
// get_current_loop_context()/set_current_loop_context()) used to have
// default bodies backed by members declared right here - moved onto
// hosted_stdcpp instead, alongside its other overrides, so this class
// stays what its name says: an interface, not a partial implementation
// with some state pre-supplied. A future bare-metal backend implements
// every one of these itself, the same as it already had to for now()/
// sleep_until()/assert_failure()/vprintdbg(); it just no longer gets two
// of them "for free."
class interface {
public:
  interface() = default;
  interface(const interface&) = delete;
  auto operator=(const interface&) -> interface& = delete;
  interface(interface&&) = delete;
  auto operator=(interface&&) -> interface& = delete;
  virtual ~interface() = default;

  [[nodiscard]] virtual auto now() const noexcept -> std::chrono::steady_clock::time_point = 0;

  // Blocks the calling thread until `deadline`, or returns immediately if
  // it has already passed - est::loop's answer to "how do I wait for the
  // next timer" without a busy-loop, same reasoning as now()/
  // assert_failure() being platform hooks: a test fake overrides this to
  // advance its own fake clock instead of actually blocking, so a loop
  // test exercising real timer-driven wakeups runs instantly instead of
  // for real wall-clock seconds (est/tests/loop_tests.cpp).
  virtual void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept = 0;

  // Reports a failed est::check() and terminates - the platform's answer
  // to "what actually happens when a check fails," same reasoning as
  // now() being the answer to "what time is it": a bare-metal backend,
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
  // immediately after, instead of measuring the gap itself with two
  // now() calls the way an earlier version did. Moved here, onto
  // platform, for the same reason now()/sleep_until() are platform
  // hooks rather than est::loop calling std::chrono/std::this_thread
  // directly: "how do we know a callback ran long" is a policy a backend
  // should get to answer for itself. hosted_stdcpp's own override just
  // records now() into a member for its detect_loop_stall() to compare
  // against - the only strategy that makes sense for a single-threaded,
  // synchronous-checkpoint backend. A future backend could override both
  // this and detect_loop_stall() to run a watchdog on a background thread
  // instead, checking for (and reporting) a stall in parallel while the
  // callback is still running, rather than only finding out once it
  // returns.
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
  // long_running_threshold (docs/PLAN.md, M3) stays the single source of
  // truth for the value, unchanged by which backend is installed.
  virtual void detect_loop_stall(std::chrono::steady_clock::duration threshold) const noexcept = 0;

  // est::loop's own "current loop" slot (issue #30) - held wherever the
  // installed interface implementation keeps it (hosted_stdcpp's own
  // member, below), rather than as a free-standing global living
  // alongside current_instance further down: :platform already tracks
  // "the current one" for interface itself via current_instance/
  // instance(), so this reuses that same idea instead of inventing a
  // parallel piece of global state. See est::loop's own top comment for
  // why this exists at all, and why it's deliberately not thread_local
  // (docs/PLAN.md's bare-metal embedded port stretch goal - freestanding,
  // no OS - may have no well-defined thread_local support).
  //
  // Opaque void*, not est::loop* - this partition sits below :loop in the
  // module dependency DAG (this file's own top comment) precisely so
  // :loop can depend on :platform and never the other way around; naming
  // est::loop here would invert that. est::loop performs the cast on both
  // sides, safe by construction rather than by RTTI: the only two call
  // sites that ever touch this are est::loop's own constructor (stores
  // `this`) and destructor (stores `nullptr`), so whatever's read back is
  // always either null or a genuinely live est::loop*.
  [[nodiscard]] virtual auto get_current_loop_context() const noexcept -> void* = 0;

  virtual void set_current_loop_context(void* context) noexcept = 0;
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
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
template <class... Ts> void printdbg(std::format_string<Ts...> fmt, Ts&&... args) noexcept {
  try {
    instance().vprintdbg(fmt.get(), std::make_format_args(args...));
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (...) {
  }
}

class hosted_stdcpp final : public interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return std::chrono::steady_clock::now();
  }

  void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept override {
    std::this_thread::sleep_until(deadline);
  }

  // std::vprint_unicode(), not std::println(): this is the type-erased
  // half of the split printdbg()/interface::vprintdbg() (platform.cppm's
  // own doc comments) exists for - fmt/args already arrive pre-erased via
  // std::format_args, exactly what std::vprint_unicode() itself takes, so
  // there's no formatting left for this override to do beyond handing
  // both straight through to std::cerr.
  void vprintdbg(std::string_view fmt, std::format_args args) const noexcept override {
    // Same reasoning as assert_failure()'s own try/catch below:
    // std::vprint_unicode() can throw (a format error, or an I/O
    // failure), swallowed here rather than escaping this noexcept
    // method - printdbg() promises best-effort, nothrow behavior to its
    // own caller, and this is the one place actually positioned to
    // fulfill that promise for this backend.
    try {
      std::vprint_unicode(std::cerr, fmt, args);
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
  }

  // One std::println call, not several - a reviewer comment on an
  // earlier, multi-call version pointed out there's no reason to split
  // this into 2-3 separate writes when a single format string says the
  // same thing; always including `message` (even when empty, giving
  // "assertion failed:  (in func)" with a blank between the colons) is a
  // deliberate simplification, not an oversight - the empty case is rare
  // enough (every current est::check() call site either always or never
  // passes one) that a special-cased branch to avoid a stray blank isn't
  // worth the extra code on a path that only exists to report a bug.
  // Directly to std::cerr, not building an intermediate std::string
  // first: the whole point of std::print's format_string overload is
  // writing straight into the destination.
  //
  // std::cerr (an ostream), not stderr (a FILE*): unlike stderr, std::cerr
  // is a proper namespace-std entity, so it needs nothing beyond
  // `import std;` to reach - no #include, unlike the FILE*-based stderr
  // this replaced (see docs/PLAN.md). Confirmed with a standalone
  // `import std;`-only program that actually triggers this path (not
  // just compiles it) that std::cerr writes correctly with no <iostream>
  // #include anywhere in the TU - the standard library's own static
  // initialization for the standard streams isn't skipped just because
  // the include was replaced by an import.
  [[noreturn]] void assert_failure(std::string_view message,
                                   std::source_location location) const noexcept override {
    // std::println can throw (std::format_error, or an I/O failure) -
    // caught and discarded rather than left to escape this noexcept
    // function: aborting either way is the whole point of
    // assert_failure, so a best-effort diagnostic isn't worth preferring
    // one termination path over another. Same pattern
    // examples/hello_world/main.cpp already needs around its own
    // std::println call, for the same reason.
    try {
      std::println(std::cerr,
                   "{}:{}: assertion failed: {} (in {})",
                   location.file_name(),
                   location.line(),
                   message,
                   location.function_name());
      // Deliberately empty: std::abort() unconditionally follows below
      // regardless of whether the diagnostic above printed successfully,
      // so there's nothing to handle or re-throw here.
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
    std::abort();
  }

  // interface::reset_loop_stall_detection()'s own doc comment explains
  // the "why" - this override is the "one sensible default nearly every
  // backend can just inherit" that comment describes, just no longer
  // literally inherited (interface itself holds no state to inherit) now
  // that every implementation, hosted_stdcpp included, provides its own.
  void reset_loop_stall_detection() noexcept override { stall_start_ = now(); }

  void detect_loop_stall(std::chrono::steady_clock::duration threshold) const noexcept override {
    const auto elapsed = now() - stall_start_;
    if (elapsed > threshold) {
      printdbg("est::loop: a continuation took {}ms (> {}ms threshold) to run",
               std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
               std::chrono::duration_cast<std::chrono::milliseconds>(threshold).count());
    }
  }

  // interface::get_current_loop_context()'s own doc comment explains what
  // this slot is for and why it's an opaque void*. Non-virtual on
  // interface would have worked identically for this specific backend,
  // but a bare-metal backend might reasonably want to answer "hold a
  // pointer, hand it back" differently too (e.g. a fixed static slot with
  // no heap/global initialization order to worry about) - now that
  // interface carries no state of its own for any method, there's no
  // reason to single these two out as the one non-overridable pair.
  [[nodiscard]] auto get_current_loop_context() const noexcept -> void* override {
    return current_loop_context_;
  }

  void set_current_loop_context(void* context) noexcept override {
    current_loop_context_ = context;
  }

private:
  std::chrono::steady_clock::time_point stall_start_;
  void* current_loop_context_ = nullptr;
};

} // namespace est::platform

// Not part of est::platform's exported surface (unlike future.cppm's own
// est::detail, this one is nested under est::platform specifically since
// it's platform-local state, not a framework-wide implementation detail)
// - a plain, non-exported `namespace est::platform::detail` here, so
// `default_instance`/`current_instance` stay reachable only from within
// this module, not assignable by any `import est;` consumer bypassing
// instance()/override_instance() below.
namespace est::platform::detail {
// False positive below: bugprone-throwing-static-initialization flags
// hosted_stdcpp's implicit default constructor as "possibly throwing"
// purely because it now has non-static data members of its own
// (stall_start_/current_loop_context_, holding what interface's own
// virtual methods describe) - it doesn't actually analyze whether those
// members' own default construction can throw (a
// std::chrono::steady_clock::time_point and a void* both trivially/
// noexcept default-construct), it just treats "no longer a literally
// empty class" as enough to warn. Confirmed by testing: any non-static
// member here at all triggers the identical warning, regardless of type.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
inline hosted_stdcpp default_instance{};
inline interface* current_instance = &default_instance;
} // namespace est::platform::detail

export namespace est::platform {

// The globally accessible platform object est::check()/est::timer_queue
// actually call through - defaults to hosted_stdcpp. Tests retarget it for
// a scope via override_instance(), below; a future bare-metal backend
// would install its own implementation here at startup instead.
[[nodiscard]] inline auto instance() noexcept -> interface& {
  return *detail::current_instance;
}

// Points instance() at `replacement` until the returned guard is
// destroyed, restoring whatever was current before - nests correctly,
// since each returned guard only remembers what it personally replaced.
// Built on est::scope_exit rather than a hand-rolled RAII type: the
// swap-then-restore shape is exactly what scope_exit already exists for
// (see its own doc comment).
//
// This mutates process-global state, which is exactly the tradeoff of
// swapping a single global object instead of threading a reference/
// template parameter through every consumer: safe for this project's
// single-threaded, serially-run Catch2 binary (never two tests touching
// the global at once), not meant for concurrent use.
[[nodiscard]] inline auto override_instance(interface& replacement) noexcept {
  interface* const previous = std::exchange(detail::current_instance, &replacement);
  return scope_exit([previous]() noexcept { detail::current_instance = previous; });
}

} // namespace est::platform
