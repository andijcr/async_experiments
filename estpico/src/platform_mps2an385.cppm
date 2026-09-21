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
// timer for uptime()/get_random_seed(), and a second CMSDK APB dual-timer
// (0x40002000) sleep_until() arms per call to back a real `wfi` low-power
// wait - never through ARM semihosting (`bkpt 0xab`), with one deliberate
// exception: terminate()'s exit-code
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
//
// vprintdbg()'s own UART output is asynchronous, interrupt-driven, and
// bounded (est::intrusive_list<T> queues whole formatted messages;
// est::spsc_ring<char> is the fixed-capacity queue a real UART0 TX
// interrupt actually drains) rather than the old plain busy-wait
// uart_write() every character - see enqueue_output()'s own doc comment
// for the full design and why assert_failure() deliberately does *not*
// use it.
namespace estpico::detail {

constexpr std::uint32_t uart0_base = 0x40004000U;
constexpr std::uint32_t uart0_data = uart0_base + 0x00U;
constexpr std::uint32_t uart0_state = uart0_base + 0x04U;
constexpr std::uint32_t uart0_ctrl = uart0_base + 0x08U;
constexpr std::uint32_t uart0_intstatus = uart0_base + 0x0CU;
constexpr std::uint32_t uart_state_tx_full = 0x1U;
constexpr std::uint32_t uart_ctrl_tx_en = 0x1U;
constexpr std::uint32_t uart_ctrl_tx_int_en = 1U << 2U;
constexpr std::uint32_t uart_intstatus_tx = 0x1U; // write 1 to ack/clear (confirmed empirically)

// NVIC: UART0's TX-complete line is IRQ1 - confirmed empirically (not
// found in any doc this project trusts blindly), by wiring every
// external IRQ slot to a shared handler that reports its own IPSR and
// observing which one fires after a single UART0_DATA write. Also
// confirmed empirically: the interrupt is edge-latched on "a
// transmission just completed" (not a sustained level tied to "TX
// buffer currently empty" - enabling TX_INT_EN on an already-idle,
// never-transmitted UART does *not* fire it on its own), and is acked by
// writing 1 to INTSTATUS - a real storm (dozens of re-fires) resulted
// from *not* acking it, and exactly one fire per completed byte resulted
// from acking it (issue #123's PR design comment has the full
// methodology and both results). Software must write the first byte to
// create that first edge; the interrupt then chains subsequent bytes on
// its own.
constexpr std::uint32_t nvic_iser0 = 0xE000E100U;
constexpr std::uint32_t nvic_icer0 = 0xE000E180U;
constexpr std::uint32_t uart0_tx_irqn = 1U;

// CMSDK APB dual-timer (0x40002000, confirmed via `info mtree` the same
// way UART0's base was) - Timer1's own register block, the only one of
// its two independent sub-timers this backend uses. Register offsets,
// control-register bit positions, and the ack-via-INTCLR requirement
// were all cross-checked against QEMU 8.2.2's own device model source
// (hw/timer/cmsdk-apb-dualtimer.c) and then confirmed empirically
// against this exact build, the same two-step methodology (a documented
// hypothesis, verified against the real binary) issue #123's PR design
// comment already used for SysTick's clock frequency: NVIC IRQ10 (found
// via the identical shared-handler/IPSR technique used for UART0 TX),
// and - like the UART TX interrupt - a real storm (200+ re-fires before
// a safety valve tripped) resulted from *not* acking T1INTCLR, exactly
// one fire resulted from acking it.
constexpr std::uint32_t dualtimer_base = 0x40002000U;
constexpr std::uint32_t t1_load = dualtimer_base + 0x00U;
constexpr std::uint32_t t1_control = dualtimer_base + 0x08U;
constexpr std::uint32_t t1_intclr = dualtimer_base + 0x0CU;
constexpr std::uint32_t t1_ctrl_oneshot = 1U << 0U;
constexpr std::uint32_t t1_ctrl_size_32bit = 1U << 1U;
constexpr std::uint32_t t1_ctrl_inten = 1U << 5U;
constexpr std::uint32_t t1_ctrl_enable = 1U << 7U;
constexpr std::uint32_t dualtimer_irqn = 10U;

// TIMCLK (the dual-timer's own input clock) measured empirically at
// ~24.98 MHz, calibrated against SysTick's own already-trusted uptime()
// rather than semihosting - both derive from the same board clock
// (QEMU's `mps.sysclk`), so this is the same clean 25 MHz SysTick itself
// measured at, and the same 40ns/tick.
constexpr std::uint32_t dualtimer_hz = 25'000'000U;
constexpr std::uint64_t ns_per_dt_tick = 1'000'000'000ULL / dualtimer_hz;
static_assert(1'000'000'000ULL % dualtimer_hz == 0, "must divide 1e9 evenly for exact ns");

[[nodiscard]] auto mmio32(std::uint32_t address) noexcept -> volatile std::uint32_t& {
  return *reinterpret_cast<volatile std::uint32_t*>(address); // NOLINT(*-reinterpret-cast)
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
// platform-specific hazard, not a framework-wide one. Safe to nest (the
// inner guard's destructor restores "still masked," not "unmasked") -
// several call sites below rely on that rather than each having to know
// whether an outer guard is already held.
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

// Synchronous, busy-wait UART writer - used only for assert_failure()'s
// own fatal diagnostic (never for vprintdbg(), which is asynchronous;
// see enqueue_output() below) and drain_uart_tx_synchronously()'s own
// final byte-by-byte flush. Wrapped in interrupt_guard: without it, this
// racing the TX ISR's own DATA/STATE access is exactly the corruption
// issue #123's PR design comment demonstrated empirically (two
// uncoordinated writers of the same MMIO registers). Safe to busy-wait
// on uart_state_tx_full while interrupts are masked: that bit clears via
// autonomous UART hardware completing a transmission, not via any
// software/interrupt action - masking only stops software from being
// *told* about it, not the hardware event itself.
void uart_putc(char c) noexcept {
  const interrupt_guard guard;
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

// --- Asynchronous, interrupt-driven UART TX (vprintdbg() only) ---
//
// One queued chunk of formatted output text - the est::intrusive_list<T>
// "buffer manager" half of the design. Entirely mainline-only: both
// enqueue_output() (below) and pump_some() (below, whether called
// directly or from pump_task()'s own spawned coroutine) run on the
// ordinary call stack - the TX ISR never touches pending_buffers or
// draining_buffer at all, only uart_tx_ring itself. Heap-allocated via
// std::make_unique (not est::current_allocator()): enqueue_output() can
// run before any est::loop is current at all (a debug diagnostic emitted
// while still bootstrapping, or est/tests/platform_tests.cpp's own
// "vprintdbg() writes... without throwing" test, which exercises this
// backend directly with no loop registered) - current_allocator() would
// assert in exactly that case, the same reason has_current_loop() is
// checked below rather than current_loop() called unconditionally.
struct tx_buffer : est::intrusive_list_node {
  explicit tx_buffer(std::string text) noexcept : data(std::move(text)) {}
  std::string data;
  std::size_t offset = 0;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline est::intrusive_list<tx_buffer> pending_buffers;
// The buffer currently being fed into uart_tx_ring, held outside
// pending_buffers itself since intrusive_list<T> only supports FIFO
// enqueue/dequeue - a buffer only partially drained (larger than the
// ring's current free space) needs somewhere to keep its own offset
// between pump_some() calls.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline std::unique_ptr<tx_buffer> draining_buffer;

// The fixed-capacity queue the TX ISR actually drains, one byte per
// completed-transmission interrupt - the one genuine ISR/mainline
// boundary in this design. est::spsc_ring<T>'s own doc comment already
// names "a future bare-metal interrupt handler" as a design target for
// this exact shape (just with producer/consumer reversed from its own
// doc wording there: here mainline/the pump task produces, the ISR
// consumes). A function-local static, not a plain inline global: its
// constructor allocates (via est::spsc_ring<T>'s own pmr allocator),
// and deferring that until first use (well after main() has started,
// once the heap is definitely live) avoids relying on global static
// initialization order before main() the way systick_wrap_count (a
// plain POD) never needed to.
constexpr std::size_t uart_tx_ring_capacity = 64;
[[nodiscard]] auto uart_tx_ring() noexcept -> est::spsc_ring<char>& {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static est::spsc_ring<char> ring(uart_tx_ring_capacity);
  return ring;
}

// Drains up to one byte from uart_tx_ring into UART0_DATA if the UART is
// currently idle - called both from the real TX-complete interrupt
// (UART0_TX_Handler, extern "C" below) and, wrapped in interrupt_guard,
// from mainline (uart_tx_kick(), to prime a cold start: the interrupt
// itself only ever fires on a completed transmission, never
// spontaneously on an idle bus - this file's own UART0 register comment
// above). Never runs concurrently with itself: the real ISR can't be
// preempted by mainline code, and interrupt_guard blocks the reverse
// direction - so despite the two call sites, uart_tx_ring only ever has
// one logical consumer at a time, preserving est::spsc_ring<T>'s
// single-consumer contract.
void pump_uart_hardware() noexcept {
  mmio32(uart0_intstatus) = uart_intstatus_tx; // ack - harmless if nothing was pending
  if ((mmio32(uart0_state) & uart_state_tx_full) != 0U) {
    return; // already mid-transmission; the real interrupt drives the next byte
  }
  if (const auto c = uart_tx_ring().try_pop()) {
    mmio32(uart0_data) = static_cast<std::uint32_t>(*c);
  }
}

void uart_tx_kick() noexcept {
  const interrupt_guard guard;
  pump_uart_hardware();
}

// Pushes as many bytes as fit right now from draining_buffer (pulling a
// fresh one from pending_buffers whenever it's null/exhausted) into
// uart_tx_ring, then kicks the hardware. Returns true once genuinely
// caught up (pending_buffers empty and draining_buffer consumed), false
// if uart_tx_ring filled up first and there's more left for a later
// call. Shared by three callers: enqueue_output()'s own no-loop-current
// fallback, pump_task_loop()'s spawned, yielding loop, and
// drain_uart_tx_synchronously()'s fatal-path flush (all below).
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
    draining_buffer.reset(); // fully queued - move on to the next buffer, if any
  }
}

// Rings once per enqueue_output() call while a loop is current - the
// persistent pump_task_loop() below (est::spawn()'d exactly once, the
// first time a loop becomes available) co_await-s this instead of being
// re-spawned per backlog episode. automatic mode + max_count = 1
// (est::binary_event) is the right shape for "wake up and check for
// work," not a counted resource: several enqueue_output() calls while
// the pump task is already busy just keep the single unit saturated
// (set()'s own doc comment - a no-op past the first, harmless) rather
// than queuing up N wakeups for M buffers pump_some() already drains
// all of in one pass regardless of how many set() calls contributed to
// the backlog. Constructible with no loop current at all (its own
// constructor, est:sync.event) - safe as a plain module-level global,
// like systick_wrap_count above, just not a POD this time.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline est::binary_event<est::EventResetMode::automatic> pump_wake_event;

// Mainline-only, no ISR access - no interrupt_guard needed. Only ever
// transitions false -> true, once, in enqueue_output() below; nothing
// ever needs to reset it back (pump_task_loop() never returns).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline bool pump_task_started = false;

// est::spawn()'d once, at Priority::high (not critical - urgent enough
// to keep debug output flowing promptly, but never allowed to starve
// Priority::critical work the way monopolizing the ready-queue would,
// yield_execution()'s own doc comment) - not per backlog episode. Just
// waits for pump_wake_event, drains everything pump_some() can reach
// (yielding to est::loop between chunks rather than spinning), then
// goes back to waiting - runs for the rest of the program once started.
auto pump_task_loop() -> est::future<void> {
  while (true) {
    co_await pump_wake_event.wait();
    while (!pump_some()) {
      co_await est::yield_execution();
    }
  }
}

// vprintdbg()'s entry point: queues `text` for asynchronous UART output
// instead of blocking the caller until every character has physically
// transmitted (the old uart_write()-based vprintdbg() did exactly that -
// fine for a rare, fatal assert_failure() message, but not for a debug
// diagnostic that might fire from inside a hot continuation).
//
// With no loop current at all (has_current_loop(), est:util.current_loop)
// - a debug diagnostic emitted while still bootstrapping, or
// est/tests/platform_tests.cpp's own "vprintdbg() writes... without
// throwing" test, which exercises this backend directly with no loop
// registered - pump_task_loop() could never be spawned (est::spawn()
// itself needs a loop) and pump_wake_event.set() would never be
// consumed either, so this drains whatever fits synchronously right
// now instead, matching interface::vprintdbg()'s own documented "must
// swallow its own failures... best-effort" contract rather than queuing
// text nothing will ever come back to send.
//
// Once a loop is current, every call defers to pump_task_loop() instead
// of also trying a synchronous fast path here: pump_wake_event.set() is
// safe to call even before the task has ever run (it only resolves
// current_loop() once a waiter is actually queued, its own doc comment)
// and pump_some() already handles "fits in one shot" - most messages -
// exactly as well from inside the coroutine as it would inline here.
void enqueue_output(std::string text) {
  auto node = std::make_unique<tx_buffer>(std::move(text));
  pending_buffers.enqueue(*node);
  node.release();
  if (!est::has_current_loop()) {
    (void)pump_some(); // caught-up/not caught-up both mean the same thing here: best effort
    return;
  }
  if (!pump_task_started) {
    pump_task_started = true;
    est::spawn(pump_task_loop(), est::Priority::high);
  }
  pump_wake_event.set();
}

// assert_failure()'s own preamble: flushes anything still queued in
// pending_buffers/draining_buffer/uart_tx_ring *before* writing the
// fatal message itself, so earlier debug context isn't silently lost
// just because terminate() is about to halt everything permanently
// right after. Holds one interrupt_guard for the whole flush (not
// per-iteration): about to terminate() anyway, so there's no cost to
// paying for it, and holding it makes this function the ring's sole
// popper for its entire duration - no race possible against the real
// ISR, which pump_some()/uart_tx_kick()'s own per-call guards only
// prevent one call at a time against.
void drain_uart_tx_synchronously() noexcept {
  const interrupt_guard guard;
  while (!pump_some()) {
  }
  while (true) {
    while ((mmio32(uart0_state) & uart_state_tx_full) != 0U) {
    } // safe under the mask - see uart_putc()'s own identical reasoning above
    const auto c = uart_tx_ring().try_pop();
    if (!c) {
      break;
    }
    mmio32(uart0_data) = static_cast<std::uint32_t>(*c);
  }
}

void start_uart_tx_interrupt() noexcept {
  mmio32(uart0_ctrl) = uart_ctrl_tx_en | uart_ctrl_tx_int_en;
  mmio32(nvic_iser0) = 1U << uart0_tx_irqn;
}

// Enables IRQ10 in the NVIC once, at construction - unlike the UART TX
// interrupt (whose CTRL enable and NVIC enable both happen once, up
// front, since it free-runs from then on), the dual-timer's own
// countdown is armed fresh by arm_dualtimer_oneshot() below on every
// sleep_until() call, not started here.
void start_dualtimer_irq() noexcept {
  mmio32(nvic_iser0) = 1U << dualtimer_irqn;
}

// Arms Timer1 for a single interrupt roughly `remaining` from now, then
// halts (ONESHOT) - sleep_until() below WFIs right after calling this,
// so this is what actually wakes it back up at (approximately) the
// right time instead of leaving WFI to wait for SysTick's own ~671ms
// wrap or an unrelated UART interrupt. Disables and re-acks before
// reloading regardless of whether a previous countdown is still
// in-flight (a WFI woken early by some other interrupt re-arms this
// mid-countdown; harmless to restart it with the freshly recomputed
// remaining time) - cheaper than tracking whether one is already
// running and only conditionally touching it.
//
// Deliberately not SysTick: SysTick's own reload value is load-bearing
// for uptime()'s own tick accounting (systick_ticks()'s formula assumes
// every wrap represents exactly one full systick_cycle_length) -
// temporarily shortening it to align with an arbitrary deadline would
// desynchronize the clock itself. A second, genuinely independent timer
// avoids that class of bug entirely, at the cost of the one MMIO
// peripheral this backend hadn't needed before.
void arm_dualtimer_oneshot(est::platform::clock::duration remaining) noexcept {
  const auto remaining_ns = static_cast<std::uint64_t>(remaining.count());
  std::uint64_t ticks = remaining_ns / ns_per_dt_tick;
  // A 0-tick countdown may never fire (SP804-style semantics don't
  // define what happens from an already-elapsed load) - round up to 1
  // rather than risk WFI never waking at all. The upper clamp is purely
  // defensive: sleep_until()'s own real deadlines are milliseconds, many
  // orders of magnitude under the ~171s a 32-bit counter at 25MHz holds.
  if (ticks == 0U) {
    ticks = 1U;
  } else if (ticks > 0xFFFF'FFFFULL) {
    ticks = 0xFFFF'FFFFULL;
  }
  mmio32(t1_control) = 0U; // stop and disable before reprogramming
  mmio32(t1_intclr) = 1U;  // ack any stale pending status (value is irrelevant)
  mmio32(t1_load) = static_cast<std::uint32_t>(ticks);
  mmio32(t1_control) = t1_ctrl_oneshot | t1_ctrl_size_32bit | t1_ctrl_inten | t1_ctrl_enable;
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

// startup.c's own vector table references this by name too, at slot
// 16+1 (IRQ1 - the UART0 TX interrupt, confirmed empirically; see this
// module's own UART0 register comment above), the same extern "C"
// linkage split SysTick_Handler above already has.
extern "C" void UART0_TX_Handler() noexcept { // NOLINT(readability-identifier-naming)
  estpico::detail::pump_uart_hardware();
}

// startup.c's own vector table references this at slot 16+10 (IRQ10 -
// the dual-timer's own interrupt line, confirmed empirically; see this
// module's own dual-timer register comment above). Only job is to ack
// (so ONESHOT mode's single fire doesn't storm the same way an unacked
// UART TX interrupt did) and, by virtue of being serviced at all, wake
// sleep_until()'s own `wfi` - nothing else needs to happen here.
extern "C" void DualTimer_Handler() noexcept { // NOLINT(readability-identifier-naming)
  estpico::detail::mmio32(estpico::detail::t1_intclr) = 1U;
}

export namespace estpico {

class platform_mps2an385 final : public est::platform::interface {
public:
  platform_mps2an385() noexcept {
    detail::start_systick();
    detail::start_uart_tx_interrupt();
    detail::start_dualtimer_irq();
  }

  [[nodiscard]] auto uptime() const noexcept -> est::platform::clock::time_point override {
    return detail::ticks_to_time_point(detail::systick_ticks());
  }

  // A real low-power wait: `wfi` (Wait For Interrupt, the standard
  // Cortex-M mechanism) halts the CPU until the next interrupt, backed by
  // detail::arm_dualtimer_oneshot() so that "next interrupt" is
  // (approximately) the actual deadline, not whatever unrelated interrupt
  // happens to fire next. WFI can wake on *any* enabled interrupt though
  // (SysTick's own ~671ms wrap, a UART TX completion, ...), so this still
  // rechecks uptime() against `deadline` in a loop rather than trusting a
  // single wake - re-arming the dual-timer with the freshly recomputed
  // remaining time each time it loops. A deadline already in the past
  // returns immediately without ever touching the timer at all.
  //
  // Not SysTick-based: arm_dualtimer_oneshot()'s own doc comment has the
  // reasoning (SysTick's reload is load-bearing for uptime()'s own tick
  // accounting; a second, independent timer avoids desynchronizing the
  // clock to align a wakeup to an arbitrary deadline).
  void sleep_until(est::platform::clock::time_point deadline) const noexcept override {
    while (true) {
      const auto current = uptime();
      if (current >= deadline) {
        return;
      }
      detail::arm_dualtimer_oneshot(deadline - current);
      __asm__ volatile("wfi");
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

  // Asynchronous, interrupt-driven (detail::enqueue_output()'s own doc
  // comment has the full design) - unlike the fatal assert_failure()
  // path below, a debug diagnostic has no reason to block its caller
  // until every character has physically left the UART.
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

  // Deliberately synchronous, not routed through enqueue_output(): about
  // to terminate() unconditionally right after, so there's no later
  // point at which a queued-but-not-yet-transmitted message would ever
  // actually reach the wire - drain_uart_tx_synchronously() first
  // flushes anything vprintdbg() already had in flight (so earlier debug
  // context isn't lost), then this writes its own message the same
  // guaranteed way.
  [[noreturn]] void assert_failure(std::string_view message,
                                   std::source_location location) const noexcept override {
    detail::drain_uart_tx_synchronously();
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
