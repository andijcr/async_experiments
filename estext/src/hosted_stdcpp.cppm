export module estext;

import est;
import std;

// estext: a genuinely separate module from est's own core (not one of
// est's own partitions, re-exported or otherwise) - this is where a
// concrete platform::interface implementation that needs real
// hosted-OS/libc++ facilities (std::chrono, std::this_thread, std::cerr)
// belongs, kept out of est itself entirely. `import est;` alone gives a
// consumer the complete framework with zero trace of hosted_stdcpp; a
// consumer that wants a working, ready-to-use backend opts in with a
// second import: `import est; import estext;`. A future bare-metal
// backend would be its own similarly separate module.
namespace estext::detail {

// est::platform::clock (est/src/platform/platform.cppm's own doc comment
// has the full reasoning) shares std::chrono::steady_clock's
// representation and epoch by construction - every value this backend
// ever hands out comes directly from steady_clock::now() below, so
// converting between the two is a pure reinterpretation of the same
// nanosecond count, never a real unit/epoch conversion.
[[nodiscard]] auto to_platform_clock(std::chrono::steady_clock::time_point tp) noexcept
    -> est::platform::clock::time_point {
  return est::platform::clock::time_point{tp.time_since_epoch()};
}

[[nodiscard]] auto to_steady_clock(est::platform::clock::time_point tp) noexcept
    -> std::chrono::steady_clock::time_point {
  return std::chrono::steady_clock::time_point{tp.time_since_epoch()};
}

} // namespace estext::detail

export namespace estext {

class hosted_stdcpp final : public est::platform::interface {
public:
  [[nodiscard]] auto uptime() const noexcept -> est::platform::clock::time_point override {
    return detail::to_platform_clock(std::chrono::steady_clock::now());
  }

  // std::this_thread::sleep_until() needs a real std::chrono clock (one
  // with its own working now(), used internally to retry past spurious
  // wakeups - std::chrono::steady_clock::now() itself, not this backend's
  // own uptime()) - est::platform::clock deliberately isn't one (its own
  // doc comment), so this converts back to the real steady_clock this
  // backend actually sources every value from.
  void sleep_until(est::platform::clock::time_point deadline) const noexcept override {
    std::this_thread::sleep_until(detail::to_steady_clock(deadline));
  }

  // std::random_device itself can throw (implementation-defined, if no
  // entropy source is available) - caught the same way assert_failure()/
  // vprintdbg() below already guard their own fallible std:: calls,
  // falling back to uptime()'s own bit pattern (never truly random, but at
  // least not identical across process runs) rather than letting this
  // noexcept method terminate the program over something only ever used
  // for jitter.
  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override {
    try {
      std::random_device dev;
      return (static_cast<std::uint64_t>(dev()) << 32) | dev();
    } catch (...) {
      return static_cast<std::uint64_t>(uptime().time_since_epoch().count());
    }
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

  // One std::println call, writing directly to std::cerr rather than
  // building an intermediate std::string first. Always includes
  // `message` even when empty (giving "assertion failed:  (in func)"
  // with a blank between the colons) rather than special-casing it - the
  // empty case is rare enough that it isn't worth the extra code on a
  // path that only exists to report a bug.
  //
  // std::cerr (an ostream), not stderr (a FILE*): std::cerr is a proper
  // namespace-std entity, reachable via plain `import std;` with no
  // #include needed.
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
  // the "why" - this is that method's one sensible default nearly every
  // backend can just implement the same way.
  void reset_loop_stall_detection() noexcept override { stall_start_ = uptime(); }

  void detect_loop_stall(est::platform::clock::duration threshold) const noexcept override {
    const auto elapsed = uptime() - stall_start_;
    if (elapsed > threshold) {
      est::platform::printdbg(
          "est::loop: a continuation took {}ms (> {}ms threshold) to run",
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(threshold).count());
    }
  }

private:
  est::platform::clock::time_point stall_start_;
};

} // namespace estext
