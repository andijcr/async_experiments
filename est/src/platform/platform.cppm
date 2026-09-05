export module est:platform;

import std;
import :util.scope_exit;

// The hosted-Linux platform backend, and the runtime-polymorphic seam it
// plugs into. `interface` is what est::check()/est::timer_queue actually
// need from "the platform" - a monotonic clock, and an answer to "what
// happens when a check fails." `hosted_linux` is the only implementation
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
};

class hosted_linux final : public interface {
public:
  [[nodiscard]] auto now() const noexcept -> std::chrono::steady_clock::time_point override {
    return std::chrono::steady_clock::now();
  }

  void sleep_until(std::chrono::steady_clock::time_point deadline) const noexcept override {
    std::this_thread::sleep_until(deadline);
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
inline hosted_linux default_instance{};
inline interface* current_instance = &default_instance;
} // namespace est::platform::detail

export namespace est::platform {

// The globally accessible platform object est::check()/est::timer_queue
// actually call through - defaults to hosted_linux. Tests retarget it for
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
