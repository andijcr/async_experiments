module;

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "pico/time.h"
#include "pico/types.h"

export module estrp2040;

export import :serial;

import est;
import std;

// estrp2040: a fifth est::platform::interface backend - real Raspberry Pi
// Pico (RP2040) hardware, core0 only (docs/PLAN.md's own entry for this
// backend has the full design context). Unlike estmsp (QEMU's
// mps2-an385), every peripheral here is driven through pico-sdk's own
// high-level C API (hardware_timer/hardware_uart/hardware_gpio/
// hardware_irq - vendored as a pinned submodule,
// examples/rp2040/third_party/pico-sdk) rather than hand-poked MMIO
// registers: pico-sdk already owns clock/reset bring-up
// (runtime_init()/clocks_init(), run automatically before main() via its
// own crt0) and the register-level details, so this module only needs to
// consume a working TIMER/UART0, not bring them up itself.
//
// A global module fragment (the `module;` block above) textually
// #includes pico-sdk's own C headers - the standard way to pair C++20
// modules with a non-modularized C/C++ library, same shape any other
// mixed module/legacy-header translation unit in this position would
// need; nothing else in this codebase happens to need it yet since every
// other backend either has no third-party C dependency (estmsp) or only
// needs `import std;` (every module).
//
// uptime()/get_random_seed() are backed by RP2040's TIMER peripheral
// (pico-sdk's time_us_64() - a genuinely free-running 64-bit microsecond
// counter, TIMELR/TIMEHR read ordering handled by pico-sdk itself), no
// wrap-counting ISR needed at all (simpler than estmsp's own
// systick_wrap_count scheme - RP2040's hardware counter is already
// 64-bit, unlike Cortex-M3's 24-bit SysTick). interruptible_sleep_until()
// arms one of TIMER's 4 hardware ALARM comparators (pico-sdk's
// hardware_alarm_* API, which registers its own IRQ dispatch - no manual
// vector-table wiring needed) and `wfi`s, the same "real hardware
// interrupt architecture makes wake()/wake_all() correct no-ops" shape
// estmsp's own interruptible_sleep_until() doc comment already explains.
//
// vprintdbg()'s own UART0 output is asynchronous/interrupt-driven,
// mirroring estmsp's design (est::intrusive_list<T> queues whole
// formatted messages; est::spsc_ring<char> is the fixed-capacity queue
// the TX interrupt actually drains) - but RP2040's UART0 is a real PL011
// (not CMSDK's UART), whose TX interrupt is *level*-triggered on "FIFO
// fill level below threshold," not edge-triggered on "one transmission
// just completed": leaving it permanently unmasked the way estmsp does
// would storm the moment the ring runs dry (the level condition stays
// true with nothing left to send). pump_uart_hardware() below instead
// masks the TX interrupt itself the instant it drains the ring, and
// every mainline refill path re-enables it before kicking - see its own
// doc comment for the detail.
//
// No terminate()-via-semihosting path: unlike estmsp (QEMU-only, whose
// terminate() exists purely to report a checkable process exit code
// under CI), this backend targets real, unattended hardware with no
// debug host guaranteed attached - assert_failure() halts by masking
// interrupts and looping on `wfi` forever after reporting its message,
// the same "no way back, park safely" contract every other bare-metal
// backend's fatal path ultimately reduces to when there's nothing to
// report an exit code *to*.
namespace estrp2040::detail {

// A short critical section - PRIMASK save/restore, identical to
// estmsp::detail::interrupt_guard (same Cortex-M PRIMASK mechanism,
// available on RP2040's own Cortex-M0+ core the same as estmsp's
// Cortex-M3) - around anything shared between mainline/spawned code and
// the UART0 IRQ handler below. See that class's own doc comment
// (estmsp/src/platform_mps2an385.cppm) for the full reasoning; not
// shared code between the two modules since each is otherwise
// independent and this is a handful of lines.
class interrupt_guard {
public:
  // saved_primask_ is written by the inline asm's own output operand
  // below, not a member-initializer-list the check can see.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
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

[[nodiscard]] auto ticks_to_time_point(std::uint64_t us_since_boot) noexcept
    -> est::platform::clock::time_point {
  return est::platform::clock::time_point{std::chrono::microseconds{us_since_boot}};
}

// --- Sleep/wake: one hardware ALARM comparator ---
//
// Alarm 0 of RP2040's 4 - this backend is the only thing on core0 that
// ever calls hardware_alarm_claim()/uses TIMER's alarm IRQs, so which of
// the 4 is arbitrary; claimed once, for the process lifetime.
constexpr std::uint32_t sleep_alarm_num = 0;

// The callback pico-sdk's own IRQ dispatch invokes when the armed alarm
// fires. Does nothing at all: firing the IRQ is the entire point (it
// wakes the `wfi` in interruptible_sleep_until() below) - nothing further
// needs to happen inside it, the same reasoning estmsp's own
// DualTimer_Handler has for being just an ack with no other body.
void alarm_fired(std::uint32_t /*alarm_num*/) noexcept {}

// Arms `sleep_alarm_num` for (approximately) `deadline`, replacing
// whatever target was previously set. Returns true if pico-sdk's own
// hardware_alarm_set_target() reports the target was already "missed" -
// either already in the past, or too close to schedule a real hardware
// timeout for - in which case the caller should not `wfi` at all (the
// deadline has, for practical purposes, already arrived).
[[nodiscard]] auto arm_alarm(est::platform::clock::time_point deadline) noexcept -> bool {
  const auto target_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(deadline.time_since_epoch()).count());
  absolute_time_t target{}; // NOLINT(cppcoreguidelines-pro-type-member-init) - set by
                            // update_us_since_boot below
  update_us_since_boot(&target, target_us);
  return hardware_alarm_set_target(sleep_alarm_num, target);
}

// --- Asynchronous, interrupt-driven UART0 TX (vprintdbg() only) ---
//
// One queued chunk of formatted output text - identical shape and
// reasoning to estmsp::detail::tx_buffer (est/src/platform_mps2an385.cppm's
// own doc comment has the full "why heap-allocated via std::make_unique,
// why mainline-only" reasoning, unchanged here): both enqueue_output()
// and pump_some() below run on the ordinary call stack; the UART0 IRQ
// handler never touches pending_buffers/draining_buffer, only
// uart_tx_ring.
struct tx_buffer : est::intrusive_list_node {
  explicit tx_buffer(std::string text) noexcept : data(std::move(text)) {}
  std::string data;
  std::size_t offset = 0;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline est::intrusive_list<tx_buffer> pending_buffers;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline std::unique_ptr<tx_buffer> draining_buffer;

constexpr std::size_t uart_tx_ring_capacity = 64;
// est::spsc_ring<char>'s constructor is the only thing that can throw
// here (a one-time allocation for a fixed-size ring); a failure at this
// point is unrecoverable on this target regardless, so terminating via
// the noexcept violation is an acceptable outcome, not a bug to route
// around.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] auto uart_tx_ring() noexcept -> est::spsc_ring<char>& {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static est::spsc_ring<char> ring(uart_tx_ring_capacity);
  return ring;
}

void set_tx_irq_enabled(bool enabled) noexcept {
  uart_set_irq_enables(uart0, /*rx_has_data=*/false, /*tx_needs_data=*/enabled);
}

// Drains uart_tx_ring into UART0's TX FIFO for as long as it's writable,
// stopping either when the FIFO is full (more room will free up as
// hardware transmits - the interrupt fires again on its own once the
// level condition is met, so nothing further to do here) or the ring
// runs dry. In the latter case, masks the TX interrupt itself before
// returning: PL011's TXIM is *level*-triggered on "FIFO fill below
// threshold," not edge-triggered on "one byte finished" the way CMSDK's
// UART (estmsp's target) is - leaving it unmasked with nothing left to
// send would re-fire continuously (the level condition stays true
// forever once idle), an interrupt storm estmsp's own design never had
// to guard against. Called both from the real IRQ (uart0_irq_handler
// below) and, wrapped in interrupt_guard, from mainline (uart_tx_kick())
// to prime a cold start or resume after the ring was refilled - see
// uart_tx_kick()'s own doc comment for why re-enabling the interrupt is
// that caller's job, not this function's.
void pump_uart_hardware() noexcept {
  bool ring_drained = false;
  while (uart_is_writable(uart0)) {
    const auto c = uart_tx_ring().try_pop();
    if (!c) {
      ring_drained = true;
      break;
    }
    uart_putc_raw(uart0, *c);
  }
  if (ring_drained) {
    set_tx_irq_enabled(false);
  }
}

// Mainline-only entry point: re-enables the TX interrupt (pump_uart_hardware()
// above may have masked it on a previous call, once the ring last ran
// dry) *before* draining, so newly pushed bytes this call doesn't manage
// to fit are still picked up by a real interrupt afterward, rather than
// only by the next explicit kick. Wrapped in interrupt_guard: without it,
// this would race the real ISR's own uart_is_writable()/uart_putc_raw()
// pair over the same PL011 FIFO, the identical hazard estmsp's own
// uart_tx_kick() already documents for its UART.
void uart_tx_kick() noexcept {
  const interrupt_guard guard;
  set_tx_irq_enabled(true);
  pump_uart_hardware();
}

// Identical shape and reasoning to estmsp::detail::pump_some() - pushes
// as many bytes as fit right now from draining_buffer (pulling a fresh
// one from pending_buffers whenever exhausted) into uart_tx_ring, then
// kicks the hardware. Returns true once genuinely caught up.
[[nodiscard]] auto pump_some() noexcept -> bool {
  while (true) {
    if (!draining_buffer) {
      draining_buffer.reset(pending_buffers.dequeue());
      if (!draining_buffer) {
        uart_tx_kick();
        return true;
      }
    }
    while (draining_buffer->offset < draining_buffer->data.size()) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      if (!uart_tx_ring().try_push(char{draining_buffer->data[draining_buffer->offset]})) {
        uart_tx_kick();
        return false;
      }
      ++draining_buffer->offset;
    }
    draining_buffer.reset();
  }
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline est::binary_event<est::EventResetMode::automatic> pump_wake_event;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline bool pump_task_started = false;

constexpr auto pump_backoff_delay = std::chrono::milliseconds(uart_tx_ring_capacity);

// Identical shape and reasoning to estmsp::detail::pump_task_loop() - see
// its own doc comment (estmsp/src/platform_mps2an385.cppm) for the full
// priority/stop_token/cancellation-handling rationale, unchanged here.
auto pump_task_loop(est::stop_token token) -> est::future<void> {
  try {
    while (true) {
      co_await est::with_stop(pump_wake_event.wait(), token);
      while (!pump_some()) {
        co_await est::sleep_for(pump_backoff_delay, token);
      }
    }
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (const est::operation_cancelled&) {
    // request_stop() fired - exit cleanly, no more work to pick up.
  }
}

[[nodiscard]] auto pump_stop_source() noexcept -> est::stop_source& {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static est::stop_source source;
  return source;
}

// Identical shape and reasoning to estmsp::detail::enqueue_output() - see
// its own doc comment for the full "why synchronous fallback with no
// loop current" rationale, unchanged here.
void enqueue_output(std::string text) {
  auto node = std::make_unique<tx_buffer>(std::move(text));
  pending_buffers.enqueue(*node);
  node.release();
  if (!est::has_current_loop()) {
    (void)pump_some();
    return;
  }
  if (!pump_task_started) {
    est::spawn([] { return pump_task_loop(pump_stop_source().get_token()); }, est::Priority::high);
    pump_task_started = true;
  }
  pump_wake_event.set();
}

// Synchronous, busy-wait UART writer - used only for assert_failure()'s
// own fatal diagnostic, never for vprintdbg() (identical split to
// estmsp::detail::uart_write()/uart_putc()). Safe to busy-wait on
// uart_is_writable() while interrupts are masked: that condition clears
// via autonomous UART hardware completing a transmission, not via any
// software/interrupt action.
void uart_write_blocking_locked(std::string_view text) noexcept {
  const interrupt_guard guard;
  for (char c : text) {
    while (!uart_is_writable(uart0)) {
    }
    uart_putc_raw(uart0, c);
  }
}

// assert_failure()'s own preamble - flushes anything still queued before
// writing the fatal message itself, identical reasoning to
// estmsp::detail::drain_uart_tx_synchronously().
void drain_uart_tx_synchronously() noexcept {
  const interrupt_guard guard;
  while (!pump_some()) {
  }
  while (uart_is_writable(uart0)) {
    const auto c = uart_tx_ring().try_pop();
    if (!c) {
      break;
    }
    uart_putc_raw(uart0, *c);
  }
}

// extern "C" linkage: registered at runtime via irq_set_exclusive_handler()
// below (pico-sdk's own IRQ dispatch mechanism), not referenced from a
// hand-written vector table the way estmsp's handlers are - pico-sdk's
// own crt0/vector table already routes every NVIC IRQ through this
// registration API, so no vector-table wiring belongs to this module at
// all.
extern "C" void uart0_irq_handler() noexcept { // NOLINT(readability-identifier-naming)
  pump_uart_hardware();
}

void init_uart0() noexcept {
  uart_init(uart0, PICO_DEFAULT_UART_BAUD_RATE);
  gpio_set_function(PICO_DEFAULT_UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(PICO_DEFAULT_UART_RX_PIN, GPIO_FUNC_UART);
  uart_set_fifo_enabled(uart0, true);
  irq_set_exclusive_handler(UART0_IRQ, uart0_irq_handler);
  irq_set_enabled(UART0_IRQ, true);
  // TX interrupt starts masked (set_tx_irq_enabled(true) above only
  // happens from uart_tx_kick(), the first time there's ever anything to
  // send) - matches pump_uart_hardware()'s own "mask once idle" invariant
  // from the very start, rather than starting enabled and immediately
  // storming with nothing queued yet.
}

void init_sleep_alarm() noexcept {
  hardware_alarm_claim(sleep_alarm_num);
  hardware_alarm_set_callback(sleep_alarm_num, alarm_fired);
}

} // namespace estrp2040::detail

export namespace estrp2040 {

class platform_rp2040 final : public est::platform::interface {
public:
  platform_rp2040() noexcept {
    detail::init_uart0();
    detail::init_sleep_alarm();
  }

  [[nodiscard]] auto uptime() const noexcept -> est::platform::clock::time_point override {
    return detail::ticks_to_time_point(time_us_64());
  }

  // A real low-power wait: `wfi` halts the core until the next interrupt,
  // backed by detail::arm_alarm() so that "next interrupt" is
  // (approximately) the actual deadline - identical shape and reasoning
  // to estmsp::platform_mps2an385::interruptible_sleep_until() (its own
  // doc comment has the full "exactly one wfi, not a retry loop; this is
  // what makes wake() a correct no-op" rationale, unchanged here).
  void interruptible_sleep_until(est::platform::clock::time_point deadline) noexcept override {
    if (uptime() >= deadline) {
      return;
    }
    if (detail::arm_alarm(deadline)) {
      return; // already missed by the time the hardware target was set
    }
    __asm__ volatile("wfi");
  }

  // Genuine no-ops, identical reasoning to estmsp's own: any real
  // hardware interrupt already unblocks a currently-executing wfi for
  // free, and this backend only ever drives the one loop on core0.
  void wake(est::platform::interface::WakeId /*id*/) noexcept override {}
  void wake_all() noexcept override {}

  // No hardware entropy source is wired up yet (pico-sdk's own
  // `pico_rand` library, built on the ROSC, would be a real upgrade path
  // - not linked by this first pass). time_us_64() changes on every call,
  // meeting the same documented bar as estmsp's own choice
  // (est::platform::interface::get_random_seed()'s own doc comment:
  // "doesn't need to be cryptographically secure, or even high-quality -
  // only needs to differ from the last draw").
  [[nodiscard]] auto get_random_seed() const noexcept -> std::uint64_t override {
    return time_us_64();
  }

  void vprintdbg(std::string_view fmt, std::format_args args) const noexcept override {
#ifdef __cpp_exceptions
    try {
      detail::enqueue_output(std::vformat(fmt, args) + "\n");
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
#else
    detail::enqueue_output(std::vformat(fmt, args) + "\n");
#endif
  }

  // Halts forever after reporting its message - this module's own top
  // comment has the "no semihosting/checkable-exit-code path on real
  // hardware" reasoning. Interrupts stay masked throughout: about to park
  // permanently anyway, and this keeps the UART0 IRQ handler from
  // touching anything mid-report the same way estmsp's identical
  // reasoning already covers for its own assert_failure().
  [[noreturn]] void assert_failure(std::string_view message,
                                   std::source_location location) const noexcept override {
    detail::drain_uart_tx_synchronously();
    const detail::interrupt_guard guard;
#ifdef __cpp_exceptions
    try {
      detail::uart_write_blocking_locked(std::format("{}:{}: assertion failed: {} (in {})\n",
                                                     location.file_name(),
                                                     location.line(),
                                                     message,
                                                     location.function_name()));
      // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
#else
    detail::uart_write_blocking_locked(std::format("{}:{}: assertion failed: {} (in {})\n",
                                                   location.file_name(),
                                                   location.line(),
                                                   message,
                                                   location.function_name()));
#endif
    for (;;) {
      __asm__ volatile("wfi");
    }
  }

  void reset_loop_stall_detection() noexcept override { stall_start_ = uptime(); }

  void detect_loop_stall(est::platform::clock::duration threshold) const noexcept override {
    const auto elapsed = uptime() - stall_start_;
    if (elapsed > threshold) {
      est::platform::printdbg(
          "estrp2040: a continuation took {}ms (> {}ms threshold) to run",
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(threshold).count());
    }
  }

private:
  est::platform::clock::time_point stall_start_;
};

// Identical reasoning to estmsp::request_uart_tx_pump_stop() - a real,
// correct building block; no call site in this codebase yet (same open
// question that module's own doc comment already describes).
inline void request_uart_tx_pump_stop() noexcept {
  detail::pump_stop_source().request_stop();
}

} // namespace estrp2040
