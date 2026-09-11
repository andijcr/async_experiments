export module est:platform;

import std;
import :util.scope_exit;

// The runtime-polymorphic seam est::check()/est::timer_queue actually
// need from "the platform" - a monotonic clock, and an answer to "what
// happens when a check fails." `interface` is deliberately the *only*
// thing this module knows about a platform: a pure abstract base with no
// concrete backend of its own. The one concrete backend, `hosted_stdcpp`,
// lives in `estext`, a wholly separate module (`estext/src/
// hosted_stdcpp.cppm`) - it needs to *construct* an actual est::loop for
// its "current loop" fallback (get_current_loop_context() below), which
// `:platform` itself can't do: `:platform` sits below `:loop` in est's
// own internal module DAG and never imports it, keeping that DAG a
// strict one-way street. `estext` isn't bound by that internal DAG - it
// simply `import est;`s the finished product.
//
// This is virtual dispatch through a single global object, not a
// compile-time template parameter - est::timer_queue has no Platform
// template parameter, at the cost of one indirect call per now()/
// assert_failure() instead of a direct one. A future bare-metal backend
// is a second `final` class implementing `interface`, in its own module,
// installed as the global instance at startup - not a framework redesign
// either way.

// Forward declaration only, deliberately `export`ed: an exported forward
// declaration in one partition of a module attaches to the real
// definition in another partition of the *same* module, with no import
// edge needed between them (a plain, non-exported declaration does not
// work this way - Clang diagnoses it as redeclaring an entity with
// module-private linkage). This lets get_current_loop_context()/
// set_current_loop_context() below name `est::loop*` directly, with no
// `:loop` import and no `void*`+cast. Naming the type is fine; only
// constructing or otherwise completing one would invert the DAG, and no
// method here needs to - `estext` is the module that actually completes
// the type.
export namespace est {
class loop;
}

export namespace est::platform {

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
  // immediately after. Moved onto platform for the same reason now()/
  // sleep_until() are platform hooks rather than est::loop calling
  // std::chrono/std::this_thread directly: "how do we know a callback
  // ran long" is a policy a backend should get to answer for itself.
  // hosted_stdcpp's own override just records now() into a member for
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
  virtual void detect_loop_stall(std::chrono::steady_clock::duration threshold) const noexcept = 0;

  // est::loop's own "current loop" slot - held wherever the installed
  // interface implementation keeps it (hosted_stdcpp's own member,
  // module estext), rather than as a free-standing global living
  // alongside current_instance further down: :platform already tracks
  // "the current one" for interface itself via current_instance/
  // instance(), so this reuses that same idea. Deliberately not
  // thread_local, same as everything else in this single-threaded
  // codebase.
  //
  // A genuinely typed est::loop*, not an opaque void* - see the forward
  // declaration above for how this module names est::loop without
  // importing :loop. The only call sites that write into this slot are
  // est::loop::make_current()'s own guard (storing `this`, then
  // `nullptr` when the guard is destroyed) and, for hosted_stdcpp
  // specifically, its own lazily-constructed fallback loop.
  [[nodiscard]] virtual auto get_current_loop_context() const noexcept -> est::loop* = 0;

  virtual void set_current_loop_context(est::loop* context) noexcept = 0;
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
// import without inverting the dependency DAG. Starts null; a program's
// own `main()` installs a concrete interface via override_instance()
// before running any code that calls instance() - see
// examples/hello_world/main.cpp or est/tests/test_main.cpp.
inline interface* current_instance = nullptr;
} // namespace est::platform::detail

export namespace est::platform {

// The globally accessible platform object est::check()/est::timer_queue
// actually call through. Nothing is installed until a consumer's own
// `main()` calls override_instance() with a concrete backend (typically
// estext::hosted_stdcpp) - see detail::current_instance's own doc
// comment.
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
