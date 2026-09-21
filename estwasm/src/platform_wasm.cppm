export module estwasm;

import est;
import std;

// estwasm: a third est::platform::interface backend, a wholly separate
// module from est itself and from estext (mirroring that module's own
// split - see estext/src/hosted_stdcpp.cppm's top comment) - this is the
// "a future bare-metal backend would be its own similarly separate
// module" case docs/wiki/Architecture.md already speculated about,
// except the "bare metal" here is a browser's WebAssembly sandbox rather
// than real hardware.
//
// Every method below routes through a small, explicit set of
// `import_module("env")` JS imports rather than any hosted-OS facility
// (no std::chrono::steady_clock::now(), no std::this_thread, no
// std::cerr - none of those are meaningfully implementable against a
// wasm32-wasip1-threads sysroot the way estext's hosted_stdcpp uses
// them against a real OS): "what time is it", "how do I block", "where
// does randomness/diagnostic output come from" are all genuinely
// platform questions here too, just answered by the browser instead of
// libc++/the OS. See docs/PLAN.md's "Issue #99" entry for the full
// design this backend exists to support (a Web Worker genuinely
// blocking est::loop via Atomics.wait, WebAssembly threads sharing
// linear memory with the main thread).
//
// Built with -fno-exceptions (cmake/toolchain-wasm32.cmake's own
// comment has the full reasoning: two confirmed, open upstream LLVM
// WebAssembly-backend bugs crash this project's pinned Clang on real
// code when the real wasm exception-handling target-feature is active).
// vprintdbg()/assert_failure() below gate their own try/catch behind
// __cpp_exceptions the same way est's own future.cppm/platform.cppm do.
namespace estwasm::detail {

// now()/sleep_until() both need a monotonic millisecond clock; JS's
// performance.now() is exactly that (unlike Date.now(), which isn't
// guaranteed monotonic) - one shared import rather than duplicating the
// declaration.
extern "C" {
// NOLINTBEGIN(readability-identifier-naming) - these are the actual C
// symbol names wasm-ld resolves as imports (import_name), not est's own
// naming convention; renaming the C++ declaration wouldn't rename the
// import.
__attribute__((import_module("env"), import_name("js_now_ms"))) auto js_now_ms() -> double;

// Blocks the calling thread (via Atomics.wait on a small SharedArrayBuffer
// the JS glue owns - only valid from a Worker thread, never the main/UI
// thread, per the browser's own Atomics.wait() restriction) until
// `deadline_ms` (same epoch/units as js_now_ms()'s own return value) is
// reached, or returns immediately if it's already passed.
__attribute__((import_module("env"), import_name("js_sleep_until_ms"))) void
js_sleep_until_ms(double deadline_ms);

// One 32-bit draw of best-effort randomness (crypto.getRandomValues()-
// backed on the JS side, not Math.random() - matching hosted_stdcpp's own
// "doesn't need to be cryptographically secure, but should at least not
// be trivially predictable" bar). get_random_seed() below combines two
// draws into 64 bits, the same shape hosted_stdcpp's own
// std::random_device-based implementation uses.
__attribute__((import_module("env"), import_name("js_random_u32"))) auto js_random_u32()
    -> std::uint32_t;

// Reports a UTF-8 diagnostic string living at [ptr, ptr+len) in this
// module's own shared linear memory - the JS glue decodes it
// (TextDecoder against the shared WebAssembly.Memory buffer) and routes
// it to console.error/console.log. `ptr` is a plain wasm32 address
// (std::uint32_t, not a real pointer type) so this declaration doesn't
// need any C++ pointer-provenance care across the extern "C" boundary;
// callers below cast from a real `const char*` right at the call site.
__attribute__((import_module("env"), import_name("js_report"))) void js_report(std::uint32_t ptr,
                                                                               std::uint32_t len);
// NOLINTEND(readability-identifier-naming)
} // extern "C"

[[nodiscard]] auto ms_to_time_point(double ms) noexcept -> est::platform::clock::time_point {
  return est::platform::clock::time_point{
      std::chrono::duration_cast<est::platform::clock::duration>(
          std::chrono::duration<double, std::milli>(ms))};
}

[[nodiscard]] auto time_point_to_ms(est::platform::clock::time_point tp) noexcept -> double {
  return std::chrono::duration<double, std::milli>(tp.time_since_epoch()).count();
}

// Common to assert_failure()/vprintdbg() below: hands a std::string's own
// bytes to js_report() via one reinterpret_cast at the boundary, exactly
// analogous to how any extern "C" call passing a buffer across a
// language/ABI seam needs one.
void report(const std::string& text) noexcept {
  js_report(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(text.data())),
            static_cast<std::uint32_t>(text.size()));
}

} // namespace estwasm::detail

export namespace estwasm {

class platform_wasm final : public est::platform::interface {
public:
  [[nodiscard]] auto now() const noexcept -> est::platform::clock::time_point override {
    return detail::ms_to_time_point(detail::js_now_ms());
  }

  // Real, OS-level blocking (Atomics.wait, via js_sleep_until_ms) - the
  // whole reason this backend exists rather than reusing estext's
  // hosted_stdcpp (see this module's own top comment). Only ever safe to
  // call from the Worker thread that installed this backend; the main
  // thread's own instantiation of the same compiled module never calls
  // est::loop::run() at all (docs/PLAN.md's Issue #99 entry), so it never
  // reaches this method regardless.
  void sleep_until(est::platform::clock::time_point deadline) const noexcept override {
    detail::js_sleep_until_ms(detail::time_point_to_ms(deadline));
  }

  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override {
    return (static_cast<std::uint64_t>(detail::js_random_u32()) << 32U) | detail::js_random_u32();
  }

  // __cpp_exceptions gate - same reasoning as est's own future.cppm/
  // platform.cppm (that module's printdbg() comment has the full
  // reasoning): std::vformat() genuinely can throw when exceptions are
  // enabled, but this backend is always built with -fno-exceptions
  // (cmake/toolchain-wasm32.cmake's own comment), so the try/catch would
  // be dead code the compiler won't let this method spell.
  void vprintdbg(std::string_view fmt, std::format_args args) const noexcept override {
#ifdef __cpp_exceptions
    try {
      detail::report(std::vformat(fmt, args));
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
#else
    detail::report(std::vformat(fmt, args));
#endif
  }

  [[noreturn]] void assert_failure(std::string_view message,
                                   std::source_location location) const noexcept override {
    // Same __cpp_exceptions reasoning as vprintdbg() above - std::format
    // can throw, and __builtin_trap() below is unconditional regardless
    // of whether the diagnostic itself made it out.
#ifdef __cpp_exceptions
    try {
      detail::report(std::format("{}:{}: assertion failed: {} (in {})",
                                 location.file_name(),
                                 location.line(),
                                 message,
                                 location.function_name()));
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
#else
    detail::report(std::format("{}:{}: assertion failed: {} (in {})",
                               location.file_name(),
                               location.line(),
                               message,
                               location.function_name()));
#endif
    // __builtin_trap(), not std::abort(): a raw `unreachable` wasm
    // instruction needs nothing from wasi-libc's own signal/process-exit
    // emulation (this backend doesn't link against _WASI_EMULATED_PROCESS_CLOCKS-
    // style abort/raise support) and is unconditionally noreturn as far
    // as the compiler's concerned, matching this method's own signature.
    __builtin_trap();
  }

  void reset_loop_stall_detection() noexcept override { stall_start_ = now(); }

  void detect_loop_stall(est::platform::clock::duration threshold) const noexcept override {
    const auto elapsed = now() - stall_start_;
    if (elapsed > threshold) {
      est::platform::printdbg(
          "estwasm: a continuation took {}ms (> {}ms threshold) to run",
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(threshold).count());
    }
  }

private:
  est::platform::clock::time_point stall_start_;
};

} // namespace estwasm
