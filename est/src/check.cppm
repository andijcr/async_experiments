export module est:check;

import std;
import :platform;

export namespace est {

#ifdef NDEBUG
inline constexpr bool checks_enabled = false;
#else
inline constexpr bool checks_enabled = true;
#endif

// A function replacement for the <cassert> macro.
// std::source_location::current(), defaulted here and evaluated at each
// call site, replaces __FILE__/__LINE__.
//
// Named check(), not assert(): a function named `assert` collides with
// <cassert>'s own macro even when called fully-qualified as
// `est::assert(...)` - the preprocessor expands `assert` by raw token
// match before the compiler ever sees the `est::` qualifier, so any
// translation unit that transitively includes <cassert> fails to
// compile.
//
// Two differences from the <cassert> macro:
// - No automatic condition-stringification: a function can't see the
//   caller's source text the way a macro can via #condition. Pass an
//   explicit message instead of the old `assert(cond && "message")`
//   idiom.
// - `condition` is an ordinary function argument, so it's always
//   evaluated, even when checks_enabled is false - unlike the macro,
//   which never evaluates its argument under NDEBUG. Only matters for a
//   condition with real cost or side effects.
//
// Debug-only, same as the macro it replaces: the `if constexpr` below
// compiles the check away entirely in a build defining NDEBUG.
// Terminates via platform::instance().assert_failure() on failure and
// never returns in that case.
//
// `inline` is required, not just an ODR nicety: without it, call sites
// in other translation units still emit a real call into an empty
// function body under -O3 -DNDEBUG, since cross-TU-via-BMI inlining
// doesn't kick in on its own at that point.
inline void check(bool condition,
                  std::string_view message = {},
                  std::source_location location = std::source_location::current()) {
  if constexpr (checks_enabled) {
    if (!condition) {
      platform::instance().assert_failure(message, location);
    }
  }
}

} // namespace est
