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

// The hosted-Linux platform backend - the only est::platform
// implementation that exists so far (docs/PLAN.md, "Platform
// abstraction (HAL)"). It's a stateless policy type (static members
// only) so est::timer_queue can be templated on it and a future
// bare-metal backend, or a test fake, is a drop-in replacement rather
// than a framework change.
export namespace est::platform {

struct hosted_linux {
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  [[nodiscard]] static auto now() noexcept -> time_point { return clock::now(); }

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

  // Reports a failed est::check() and terminates - the hosted-Linux
  // backend's answer to "what actually happens when a check fails,"
  // same reasoning as now() being the backend's answer to "what time
  // is it": a future bare-metal backend, or a test fake, gets to
  // answer this differently (halt, trigger a debug break, ...) without
  // est::check() itself changing.
  //
  // std::println(stderr, ...), not std::cerr <<: this is the one place
  // in the framework that actually performs I/O, so it's also the one
  // place that should reach for std::print/println directly - everywhere
  // else, string formatting (std::format) without printing is the right
  // tool, since *whether* and *how* to emit text is a platform concern,
  // not a framework one.
  [[noreturn]] static void assert_failure(std::string_view message,
                                          std::source_location location) noexcept {
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
