export module est:check;

import std;
import :platform;

export namespace est {

#ifdef NDEBUG
inline constexpr bool checks_enabled = false;
#else
inline constexpr bool checks_enabled = true;
#endif

// A function replacement for the <cassert> macro - avoids future.cppm's
// own prior need for the assert() macro (see below for why a function
// literally named `assert` doesn't work).
// std::source_location::current(), defaulted here and evaluated at
// each call site, replaces __FILE__/__LINE__.
//
// Named check(), not assert(): a function named `assert` collides with
// <cassert>'s own macro even when called fully-qualified as
// `est::assert(...)` - the preprocessor expands `assert` by raw token
// match before the compiler ever sees the `est::` qualifier, so any
// translation unit that transitively includes <cassert> (every test
// file using Catch2 does) fails to compile. Confirmed the hard way: the
// first version of this function was literally named assert() and
// broke every test file for exactly this reason.
//
// Two differences from the <cassert> macro worth knowing about, not
// just an invisible drop-in:
// - No automatic condition-stringification: a function can't see the
//   caller's source text the way a macro can via #condition. Pass an
//   explicit message instead of relying on the old `assert(cond &&
//   "message")` idiom.
// - `condition` is an ordinary function argument, so it's always
//   evaluated, even when checks_enabled is false - unlike the macro,
//   which expands to nothing under NDEBUG and never evaluates its
//   argument at all. Only matters for a condition with real cost or
//   side effects; every current call site is a cheap query.
//
// Debug-only, same as the macro it replaces: the `if constexpr` below
// compiles the check away entirely (not just skips it at runtime) in a
// build defining NDEBUG. Terminates via
// platform::hosted_linux::assert_failure() on failure and never
// returns in that case.
void check(bool condition,
           std::string_view message = {},
           std::source_location location = std::source_location::current()) {
  if constexpr (checks_enabled) {
    if (!condition) {
      platform::hosted_linux::assert_failure(message, location);
    }
  }
}

} // namespace est
