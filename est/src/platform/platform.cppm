module;

// stderr is an ordinary extern global (glibc), not a macro - but like
// EXIT_SUCCESS/<cstdlib> and assert/<cassert> elsewhere in this codebase,
// `import std;` still doesn't make it visible: confirmed directly (Clang:
// "missing '#include <stdio.h>'; 'stderr' must be declared before it is
// used") when this file first tried std::print(stderr, ...) with only
// `import std;` in scope.
#include <cstdio>

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

  // Builds assert_failure()'s diagnostic text - split out from
  // assert_failure() itself specifically so this (the only part with any
  // real logic - the "is message empty" branch) is unit-testable.
  // assert_failure()'s own remaining body (format + print + abort) isn't:
  // it's a [[noreturn]] function that calls std::abort(), same
  // not-practically-unit-testable situation as est::check()'s failure
  // path (est/tests/check_tests.cpp).
  [[nodiscard]] static auto format_assertion_message(std::string_view message,
                                                     std::source_location location) -> std::string {
    std::string result =
        std::format("{}:{}: assertion failed", location.file_name(), location.line());
    if (!message.empty()) {
      result += std::format(": {}", message);
    }
    result += std::format(" (in {})", location.function_name());
    return result;
  }

  // std::println(stderr, ...), not std::cerr <<: this is the one place
  // in the framework that actually performs I/O, so it's also the one
  // place that should reach for std::print/println directly - everywhere
  // else, string formatting (std::format) without printing is the right
  // tool, since *whether* and *how* to emit text is a platform concern,
  // not a framework one.
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
      std::println(stderr, "{}", format_assertion_message(message, location));
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
