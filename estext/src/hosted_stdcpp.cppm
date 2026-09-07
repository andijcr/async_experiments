export module estext;

import est;
import std;

// estext: a genuinely separate module from est's own core (not one of
// est's own partitions, re-exported or otherwise) - per review, this is
// where a concrete platform::interface implementation that needs real
// hosted-OS/libc++ facilities (std::chrono, std::this_thread, std::cerr)
// belongs, kept out of est itself entirely. `import est;` alone gives a
// consumer the complete framework - est::loop, est::future<T>, est::check(),
// platform::interface - with zero trace of hosted_stdcpp: no partition
// silently re-exporting it, nothing for the linker to even consider
// pulling in. A consumer that wants a working, ready-to-use backend
// explicitly opts in with a second import: `import est; import estext;`.
// A future bare-metal backend would be its own similarly separate module,
// never needing to touch estext at all.
//
// This also means hosted_stdcpp needs no special access est.cppm's own
// partitions don't already have: `import est;` exposes the complete,
// already-defined est::loop (unlike platform.cppm's own forward
// declaration, needed there only because :platform sits *below* :loop in
// est's own internal module DAG and can't import it - est itself has no
// such restriction from the outside).
export namespace estext {

class hosted_stdcpp final : public est::platform::interface {
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
  // the "why" - this is that method's one sensible default nearly every
  // backend can just implement the same way.
  void reset_loop_stall_detection() noexcept override { stall_start_ = now(); }

  void detect_loop_stall(std::chrono::steady_clock::duration threshold) const noexcept override {
    const auto elapsed = now() - stall_start_;
    if (elapsed > threshold) {
      est::platform::printdbg(
          "est::loop: a continuation took {}ms (> {}ms threshold) to run",
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(threshold).count());
    }
  }

  // interface::get_current_loop_context()'s own doc comment
  // (platform.cppm) explains what this slot is for and why it's typed
  // `est::loop*` here. `explicit_loop_` is whatever a caller most
  // recently registered via est::make_current_loop()
  // (est:util.current_loop) - taking priority whenever set, since a
  // caller that bothered to register a specific loop clearly wants that
  // one used, not a fallback. When nothing has been explicitly
  // registered, default_loop_ is lazily constructed on first use and
  // returned instead - this is *the* payoff of hosted_stdcpp living in
  // its own module, free to depend on est (including the complete
  // est::loop) rather than being one of est's own partitions: a caller
  // that never wants to think about est::loop at all - the original
  // ergonomic goal of issue #30 - gets a genuinely working one for free,
  // driven the same way any other
  // (`est::current_loop().run_until_idle();`), rather than
  // current_loop() simply failing its own "no loop is current"
  // precondition until someone constructs one.
  //
  // Deliberately not the same slot est::make_current_loop() clears back
  // to nullptr on: default_loop_, once constructed, lives for as long as
  // whichever hosted_stdcpp instance a consumer installed does - it isn't
  // torn down and rebuilt every time an explicit registration comes and
  // goes, the way a naive single shared slot would force.
  [[nodiscard]] auto get_current_loop_context() const noexcept -> est::loop* override {
    if (explicit_loop_ != nullptr) {
      return explicit_loop_;
    }
    if (!default_loop_.has_value()) {
      default_loop_.emplace();
    }
    return &*default_loop_;
  }

  void set_current_loop_context(est::loop* context) noexcept override { explicit_loop_ = context; }

private:
  std::chrono::steady_clock::time_point stall_start_;
  est::loop* explicit_loop_ = nullptr;
  // mutable: get_current_loop_context() is const (interface's own
  // signature - every other backend answers "what's current" without
  // needing to mutate anything either), but lazily constructing the
  // fallback loop on first use, rather than eagerly at hosted_stdcpp
  // construction time, means a program that never touches the implicit
  // convenience API at all never pays for an est::loop it doesn't use.
  mutable std::optional<est::loop> default_loop_;
};

} // namespace estext
