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
// Every method below routes through a real, memory-mapped piece of
// hardware - CMSDK APB UART0 (0x40004000, confirmed via `info mtree` in
// Findings 1-2 of issue #123) for output, the Cortex-M3 core's own SysTick
// timer for uptime()/get_random_seed() - never through ARM semihosting
// (`bkpt 0xab`), with one deliberate exception: terminate()'s exit-code
// report. That's the only place this backend still talks to whatever's
// running it, and it's not part of "functioning" - a real, flashed
// firmware never calls it at all (nothing here returns from main() or
// asserts under normal operation); it exists purely so a QEMU-hosted CI
// run can report a real, checkable process exit code, the one thing this
// backend is actually scoped to support (see this module's own doc
// comment history in docs/PLAN.md for the earlier, fully
// semihosting-dependent version of uptime()/get_random_seed() this
// replaced, and why: a firmware whose basic sense of time only works
// under an attached debug host isn't "real firmware" in any useful
// sense).
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

// The Cortex-M3 core's own SysTick timer (0xE000E010, part of the
// processor itself - not a CMSDK peripheral, needs no board-specific
// address lookup) - CLKSOURCE=1 (the processor clock) measured
// empirically at ~25.000 MHz under this exact QEMU version/machine (issue
// #123's design comment has the calibration methodology: a 30-wrap
// busy-poll bracketed by semihosting's SYS_ELAPSED as an independent,
// already-trusted ground truth). 25 MHz makes each tick exactly 40ns -
// ticks_to_time_point() below is pure integer multiplication, no
// long double rounding the old semihosting-tick-frequency conversion
// needed.
constexpr std::uint32_t syst_csr = 0xE000E010U;
constexpr std::uint32_t syst_rvr = 0xE000E014U;
constexpr std::uint32_t syst_cvr = 0xE000E018U;

constexpr std::uint32_t syst_csr_enable = 1U << 0U;
constexpr std::uint32_t syst_csr_tickint = 1U << 1U;
constexpr std::uint32_t syst_csr_clksource = 1U << 2U;

// Max 24-bit reload: minimizes how often SysTick_Handler below has to run
// (once per ~671ms at 25MHz) to extend the hardware counter into the
// 64-bit tick count platform::clock promises.
constexpr std::uint32_t systick_reload = 0x00FF'FFFFU;
constexpr std::uint64_t systick_cycle_length = static_cast<std::uint64_t>(systick_reload) + 1U;
constexpr std::uint32_t systick_hz = 25'000'000U;
constexpr std::uint64_t ns_per_tick = 1'000'000'000ULL / systick_hz;
static_assert(1'000'000'000ULL % systick_hz == 0, "systick_hz must divide 1e9 evenly for exact ns");

// How many times SysTick has wrapped since start_systick() - the only
// state SysTick_Handler (extern "C", startup.c's own vector table
// references it directly) touches. `volatile`: written from interrupt
// context, read from mainline/loop context, with nothing but this
// qualifier and systick_ticks()'s own interrupt_guard between them - a
// plain, non-atomic `std::uint64_t` would be a real data race on the
// language's own terms (single core or not), the same reasoning
// est::spsc_ring<T>'s own atomics exist for its ISR/loop handoff.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline volatile std::uint64_t systick_wrap_count = 0;

void start_systick() noexcept {
  mmio32(syst_rvr) = systick_reload;
  mmio32(syst_cvr) = 0U; // any write clears SYST_CVR (and COUNTFLAG) to 0
  mmio32(syst_csr) = syst_csr_enable | syst_csr_tickint | syst_csr_clksource;
}

// A short critical section - PRIMASK save/restore ("cpsid i" masks every
// exception below priority 0, "msr primask" restores whatever the caller
// had) - around anything shared between mainline/spawned code and an ISR.
// This is a genuinely new hazard class on this backend specifically: an
// interrupt really does preempt mainline code asynchronously, even on one
// core, which is a different, real concurrency problem from the "two
// coroutines interleaved at a co_await" case est::mutex exists for
// (CLAUDE.md's "single-threaded, no atomics... don't add locking/atomics
// speculatively" doesn't cover this - it isn't speculative here).
// estpico::detail-local, not part of est itself: this is a
// platform-specific hazard, not a framework-wide one.
class interrupt_guard {
public:
  interrupt_guard() noexcept {
    __asm__ volatile("mrs %0, primask" : "=r"(saved_primask_)); // NOLINT(*-avoid-c-arrays)
    __asm__ volatile("cpsid i" ::: "memory");
  }
  interrupt_guard(const interrupt_guard&) = delete;
  auto operator=(const interrupt_guard&) -> interrupt_guard& = delete;
  interrupt_guard(interrupt_guard&&) = delete;
  auto operator=(interrupt_guard&&) -> interrupt_guard& = delete;
  ~interrupt_guard() { __asm__ volatile("msr primask, %0" : : "r"(saved_primask_) : "memory"); }

private:
  std::uint32_t saved_primask_;
};

// Pairs systick_wrap_count with SYST_CVR atomically against
// SysTick_Handler - without the critical section, a read landing in the
// tiny window between hardware auto-reloading SYST_CVR (on the count-to-
// zero clock edge) and the ISR actually running (real, if small,
// interrupt latency) would see a post-wrap CVR alongside a not-yet-
// incremented wrap_count, undercounting by one whole cycle_length until
// the ISR catches up - a real, if rare, monotonicity violation. Masking
// interrupts for the few instructions below removes the race entirely,
// at a cost cheap enough to pay on every uptime() call (a handful of
// cycles, nothing like the real host round-trip the old semihosting-based
// elapsed_ticks() paid on every call).
[[nodiscard]] auto systick_ticks() noexcept -> std::uint64_t {
  const interrupt_guard guard;
  const std::uint64_t wraps = systick_wrap_count;
  const std::uint32_t cvr = mmio32(syst_cvr) & systick_reload;
  return (wraps * systick_cycle_length) + (systick_reload - cvr);
}

[[nodiscard]] auto ticks_to_time_point(std::uint64_t ticks) noexcept
    -> est::platform::clock::time_point {
  return est::platform::clock::time_point{std::chrono::nanoseconds{ticks * ns_per_tick}};
}

// ARM semihosting (`bkpt 0xab`, r0 = operation, r1 = parameter) - kept
// only for terminate() below (SYS_EXIT_EXTENDED), not for anything this
// backend needs to actually function (see this module's own top
// comment).
[[nodiscard]] auto semihost_call(std::uint32_t operation, std::uint32_t parameter) noexcept
    -> std::uint32_t {
  register std::uint32_t r0 __asm__("r0") = operation;
  register std::uint32_t r1 __asm__("r1") = parameter;
  __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
  return r0;
}

constexpr std::uint32_t sys_exit_extended = 0x20U;
constexpr std::uint32_t adp_stopped_application_exit = 0x20026U;

// SYS_EXIT_EXTENDED (not SYS_EXIT/0x18): the only semihosting exit
// operation that actually propagates an arbitrary host process exit
// code on AArch32 (Finding 1, issue #123) - the platform module's own
// unconditional termination path, deliberately not delegating to any
// application-level semihost_exit() (this module owns "what happens
// when a check fails" end to end, the same as estwasm's __builtin_trap()
// call doesn't delegate to anything JS-side either). On real hardware
// with no debug host attached, this bkpt would fault instead of exiting
// cleanly - an accepted, pre-existing gap (this backend is QEMU-only, per
// this module's own top comment), not something this method tries to
// paper over with a fallback.
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

// startup.c's own vector table (a C translation unit) references this by
// name at slot 15 (SysTick) instead of Default_Handler - extern "C" gives
// it the plain, unmangled linkage that reference needs; the definition
// lives here, not in startup.c, since it's this backend's own clock
// state it maintains, the same ownership split _exit()/gettimeofday() etc.
// already have (defined in startup.c because picolibc/libc++ call them,
// not because startup.c has any reason to know their implementation).
extern "C" void SysTick_Handler() noexcept { // NOLINT(readability-identifier-naming)
  // Not `++systick_wrap_count`: incrementing a volatile lvalue via the
  // built-in compound operators is deprecated in C++20 - spelled as a
  // plain read/add/write instead, still one indivisible operation from
  // this single-core ISR's own point of view (nothing else ever writes
  // this variable).
  estpico::detail::systick_wrap_count = estpico::detail::systick_wrap_count + 1U;
}

export namespace estpico {

class platform_mps2an385 final : public est::platform::interface {
public:
  platform_mps2an385() noexcept {
    detail::mmio32(detail::uart0_ctrl) = detail::uart_ctrl_tx_en;
    detail::start_systick();
  }

  [[nodiscard]] auto uptime() const noexcept -> est::platform::clock::time_point override {
    return detail::ticks_to_time_point(detail::systick_ticks());
  }

  // No real low-power wait: a plain busy-poll until `deadline`. QEMU has
  // no meaningful power draw to save, and this machine's timer-IRQ wakeup
  // wiring isn't something this backend needs yet (est::loop's own run
  // loop already only calls this when it has genuinely nothing else to
  // do) - a real hardware backend targeting actual power-sensitive
  // silicon would replace this with a WFI + timer-interrupt wakeup
  // instead, without anything above this interface needing to change.
  //
  // Unlike the old semihosting-based uptime() (a real trap out to
  // whatever's running the CPU on every call, expensive enough that a
  // plain polling loop cost tens of real seconds per simulated second -
  // see docs/PLAN.md's estpico entry), systick_ticks() is a couple of
  // MMIO reads behind a short interrupt mask - cheap enough that polling
  // it directly needs no calibrated busy-spin estimate to cover the bulk
  // of the wait first. This is a straight simplification the SysTick
  // switch enabled, not a design carried over from before.
  void sleep_until(est::platform::clock::time_point deadline) const noexcept override {
    while (uptime() < deadline) {
    }
  }

  // No semihosting randomness operation exists (ARM semihosting's SYS_*
  // set has no equivalent to hosted_stdcpp's std::random_device), and
  // this backend has no hardware entropy source either - systick_ticks()
  // changes on every call (jitter's own documented bar: "doesn't need to
  // be cryptographically secure, or even high-quality - only needs to
  // differ from the last draw, never resist prediction" -
  // est::platform::interface::get_random_seed()'s own doc comment).
  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override {
    return detail::systick_ticks();
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

  void reset_loop_stall_detection() noexcept override { stall_start_ = uptime(); }

  void detect_loop_stall(est::platform::clock::duration threshold) const noexcept override {
    const auto elapsed = uptime() - stall_start_;
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
