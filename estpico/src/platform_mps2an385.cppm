export module estpico;

import est;
import std;

// estpico: a fourth est::platform::interface backend, a wholly separate
// module from est/estext/estwasm (mirroring their own split - see
// estwasm/src/platform_wasm.cppm's top comment). The "bare metal" here is
// QEMU's mps2-an385 machine (an emulated ARM MPS2 FPGA image, single
// Cortex-M3, not real hardware and not a Raspberry Pi Pico/RP2040 - see
// docs/PLAN.md's estpico entry for why the target changed and why the
// module kept its original name).
//
// Every method below routes through either the CMSDK APB UART0 peripheral
// (0x40004000, confirmed via `info mtree` in Findings 1-2 of issue #123)
// or ARM semihosting (`bkpt 0xab`) - there's no real OS underneath, so
// "what time is it"/"where does randomness come from"/"where do
// diagnostics go" are genuinely platform questions here too, the same
// reasoning estwasm's own top comment gives for routing through JS
// imports instead of std::chrono/std::this_thread/std::cerr.
//
// now()'s clock source is semihosting's SYS_ELAPSED/SYS_TICKFREQ (ops
// 0x30/0x31), not a hand-rolled CMSDK timer or Cortex-M DWT cycle-counter
// driver: verified empirically under this exact QEMU machine/version to
// return a real, monotonically increasing, nanosecond-resolution tick
// count with zero board-specific register knowledge needed - see issue
// #123's PR-design comment. This does mean now() only works when QEMU
// is invoked with `-semihosting` (true for every use this backend is
// built for: QEMU-hosted tests/CI, not real hardware), matching
// semihost_exit()'s own existing dependency on the same flag.
namespace estpico::detail {

constexpr std::uint32_t uart0_base = 0x40004000U;
constexpr std::uint32_t uart0_data = uart0_base + 0x00U;
constexpr std::uint32_t uart0_state = uart0_base + 0x04U;
constexpr std::uint32_t uart0_ctrl = uart0_base + 0x08U;
constexpr std::uint32_t uart_state_tx_full = 0x1U;
constexpr std::uint32_t uart_ctrl_tx_en = 0x1U;

[[nodiscard]] auto mmio32(std::uint32_t address) noexcept -> volatile std::uint32_t& {
  return *reinterpret_cast<volatile std::uint32_t*>(address); // NOLINT(*-reinterpret-cast)
}

void uart_putc(char c) noexcept {
  while ((mmio32(uart0_state) & uart_state_tx_full) != 0U) {
  }
  mmio32(uart0_data) = static_cast<std::uint32_t>(c);
}

void uart_write(std::string_view text) noexcept {
  for (char c : text) {
    uart_putc(c);
  }
}

// ARM semihosting (`bkpt 0xab`, r0 = operation, r1 = parameter) - the same
// mechanism examples/*/main.cpp's own semihost_exit() uses, generalized
// to any single-word-parameter operation. Returns r0's value on exit,
// which every operation used below (SYS_ELAPSED, SYS_TICKFREQ) defines as
// its own result/error code.
[[nodiscard]] auto semihost_call(std::uint32_t operation, std::uint32_t parameter) noexcept
    -> std::uint32_t {
  register std::uint32_t r0 __asm__("r0") = operation;
  register std::uint32_t r1 __asm__("r1") = parameter;
  __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
  return r0;
}

constexpr std::uint32_t sys_elapsed = 0x30U;
constexpr std::uint32_t sys_tickfreq = 0x31U;
constexpr std::uint32_t sys_exit_extended = 0x20U;
constexpr std::uint32_t adp_stopped_application_exit = 0x20026U;

// SYS_ELAPSED writes a 64-bit tick count into a caller-owned 2-word block;
// SYS_TICKFREQ returns ticks/second directly in r0. Both confirmed
// present and working under this QEMU version/machine (freq == 1e9,
// i.e. nanosecond ticks) - see this module's top comment.
[[nodiscard]] auto elapsed_ticks() noexcept -> std::uint64_t {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static std::uint32_t block[2];
  (void)semihost_call(sys_elapsed,
                      reinterpret_cast<std::uint32_t>(block)); // NOLINT(*-reinterpret-cast)
  return (static_cast<std::uint64_t>(block[1]) << 32U) | block[0];
}

// Queried once and cached: SYS_TICKFREQ answers a fixed property of the
// host running QEMU, not something that changes between calls - and each
// semihosting call is a real trap out to the host (a `bkpt` exception,
// caught and handled by QEMU itself), expensive enough that now()
// calling this on every invocation would roughly double every caller's
// cost, including sleep_until()'s own busy-poll below, for a value that
// never actually changes.
[[nodiscard]] auto tick_frequency_hz() noexcept -> std::uint32_t {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static const std::uint32_t freq = semihost_call(sys_tickfreq, 0U);
  return freq;
}

// freq_hz ticks/second -> nanoseconds, scaled generally rather than
// assuming the specific 1e9 (nanosecond-tick) frequency this QEMU
// version happens to report - a future QEMU release reporting a
// different SYS_TICKFREQ still converts correctly.
[[nodiscard]] auto ticks_to_ns(std::uint64_t ticks, std::uint32_t freq_hz) noexcept -> long double {
  return (static_cast<long double>(ticks) * 1'000'000'000.0L) / static_cast<long double>(freq_hz);
}

[[nodiscard]] auto ticks_to_time_point(std::uint64_t ticks, std::uint32_t freq_hz) noexcept
    -> est::platform::clock::time_point {
  const auto nanos = static_cast<std::uint64_t>(ticks_to_ns(ticks, freq_hz));
  return est::platform::clock::time_point{std::chrono::nanoseconds{nanos}};
}

// A trivial, compiler-can't-optimize-away spin (the `volatile` write
// blocks eliding the loop entirely) - sleep_until() below uses this to
// cover most of a wait without paying a semihosting round-trip (a real
// trap out to the host, expensive enough to matter - see this module's
// top comment) on every iteration.
void busy_spin(std::uint64_t iterations) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static volatile std::uint32_t sink = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    sink = sink + 1U;
  }
}

// Measured once (first call, cached forever after): how many nanoseconds
// of real QEMU time one busy_spin() iteration costs on this host. Doing
// this at runtime rather than hardcoding a constant - TCG emulation
// speed varies by host machine and QEMU version, and a wrong hardcoded
// guess would either undershoot (falling back to the slow now()-polling
// path anyway) or overshoot (sleep_until() waking up late).
// sleep_until()'s own now()-polling correction loop afterward is what
// makes a somewhat-approximate calibration safe either way - this only
// needs to get close, not exact.
[[nodiscard]] auto calibrated_ns_per_spin_iteration() noexcept -> long double {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static const long double result = [] {
    constexpr std::uint64_t calibration_iterations = 500'000U;
    const std::uint32_t freq = tick_frequency_hz();
    const std::uint64_t start = elapsed_ticks();
    busy_spin(calibration_iterations);
    const std::uint64_t end = elapsed_ticks();
    return ticks_to_ns(end - start, freq) / static_cast<long double>(calibration_iterations);
  }();
  return result;
}

// SYS_EXIT_EXTENDED (not SYS_EXIT/0x18): the only semihosting exit
// operation that actually propagates an arbitrary host process exit
// code on AArch32 (Finding 1, issue #123) - the platform module's own
// unconditional termination path, deliberately not delegating to any
// application-level semihost_exit() (this module owns "what happens
// when a check fails" end to end, the same as estwasm's __builtin_trap()
// call doesn't delegate to anything JS-side either).
[[noreturn]] void terminate(int code) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static std::uint32_t block[2];
  block[0] = adp_stopped_application_exit;
  block[1] = static_cast<std::uint32_t>(code);
  (void)semihost_call(sys_exit_extended,
                      reinterpret_cast<std::uint32_t>(block)); // NOLINT(*-reinterpret-cast)
  for (;;) {
  }
}

} // namespace estpico::detail

export namespace estpico {

class platform_mps2an385 final : public est::platform::interface {
public:
  platform_mps2an385() noexcept { detail::mmio32(detail::uart0_ctrl) = detail::uart_ctrl_tx_en; }

  [[nodiscard]] auto now() const noexcept -> est::platform::clock::time_point override {
    return detail::ticks_to_time_point(detail::elapsed_ticks(), detail::tick_frequency_hz());
  }

  // No real low-power wait: a busy-wait until `deadline`. QEMU has no
  // meaningful power draw to save, and this machine's CMSDK timer IRQ
  // wiring isn't something this backend needs yet (est::loop's own run
  // loop already only calls this when it has genuinely nothing else to
  // do) - a real hardware backend targeting actual power-sensitive
  // silicon would replace this with a WFI + timer-interrupt wakeup
  // instead, without anything above this interface needing to change.
  //
  // Not a plain `while (now() < deadline) {}`: now() is a semihosting
  // round-trip (this module's own top comment), expensive enough that
  // polling it on every spin iteration made a real app (three
  // est::schedule_periodic() timers, each waking every few tens of
  // milliseconds) take tens of seconds of real QEMU time to run for a
  // few seconds of simulated time. detail::busy_spin() covers the bulk
  // of the wait using detail::calibrated_ns_per_spin_iteration()'s
  // runtime-measured estimate (one now()/spin round-trip, cached
  // forever after) instead, falling back to the same now()-polling loop
  // only to correct whatever the estimate under/overshot by - a handful
  // of iterations at most once the spin has done its job, not thousands.
  void sleep_until(est::platform::clock::time_point deadline) const noexcept override {
    const auto current = now();
    if (current >= deadline) {
      return;
    }
    const long double ns_per_iteration = detail::calibrated_ns_per_spin_iteration();
    if (ns_per_iteration > 0.0L) {
      const auto remaining_ns =
          std::chrono::duration<long double, std::nano>(deadline - current).count();
      detail::busy_spin(static_cast<std::uint64_t>(remaining_ns / ns_per_iteration));
    }
    while (now() < deadline) {
    }
  }

  // No semihosting randomness operation exists (ARM semihosting's SYS_*
  // set has no equivalent to hosted_stdcpp's std::random_device) - the
  // elapsed-tick counter is host-wall-clock-correlated and changes on
  // every call, which is exactly jitter's own documented bar ("doesn't
  // need to be cryptographically secure, or even high-quality - only
  // needs to differ from the last draw, never resist prediction" -
  // est::platform::interface::get_random_seed()'s own doc comment).
  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override {
    return detail::elapsed_ticks();
  }

  void vprintdbg(std::string_view fmt, std::format_args args) const noexcept override {
#ifdef __cpp_exceptions
    try {
      detail::uart_write(std::vformat(fmt, args));
      detail::uart_write("\n");
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
#else
    detail::uart_write(std::vformat(fmt, args));
    detail::uart_write("\n");
#endif
  }

  [[noreturn]] void assert_failure(std::string_view message,
                                   std::source_location location) const noexcept override {
#ifdef __cpp_exceptions
    try {
      detail::uart_write(std::format("{}:{}: assertion failed: {} (in {})\n",
                                     location.file_name(),
                                     location.line(),
                                     message,
                                     location.function_name()));
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
#else
    detail::uart_write(std::format("{}:{}: assertion failed: {} (in {})\n",
                                   location.file_name(),
                                   location.line(),
                                   message,
                                   location.function_name()));
#endif
    detail::terminate(1);
  }

  void reset_loop_stall_detection() noexcept override { stall_start_ = now(); }

  void detect_loop_stall(est::platform::clock::duration threshold) const noexcept override {
    const auto elapsed = now() - stall_start_;
    if (elapsed > threshold) {
      est::platform::printdbg(
          "estpico: a continuation took {}ms (> {}ms threshold) to run",
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(threshold).count());
    }
  }

private:
  est::platform::clock::time_point stall_start_;
};

} // namespace estpico
