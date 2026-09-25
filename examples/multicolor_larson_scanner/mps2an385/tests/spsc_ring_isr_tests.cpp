import est;
import std;

#include <catch2/catch_test_macros.hpp>

// A genuine ISR-driven producer/consumer test for est::spsc_ring<T> -
// exercises the real hazard class its atomic acquire/release protocol
// exists for (a hardware interrupt asynchronously preempting mainline
// code mid-operation) instead of the single-threaded simulation
// est/tests/spsc_ring_tests.cpp uses or the real-OS-thread case
// est/tests/spsc_ring_thread_tests.cpp uses (neither reusable here:
// this target has no OS threads at all - EST_NO_THREADS). Lives in this
// project's own tests/, not est/tests/, since it needs real NVIC
// registers - not backend-agnostic.
//
// Mirrors the real production shape (estmsp::enqueue_output()'s
// mainline producer racing UART0_TX_Handler's ISR consumer,
// platform_mps2an385.cppm) rather than reusing that same UART0 TX
// interrupt directly: UART0 TX is tied to real hardware timing/state
// (edge-latched on "a transmission just completed," acked via
// INTSTATUS - that module's own comment), nothing this test needs. It
// self-triggers an otherwise-permanently-idle external interrupt line
// instead (IRQ6, "GPIO0" - startup.c's own vector table comment; wired
// to a weak default everywhere except this file), via the NVIC's own
// Interrupt Set-Pending Register - a real ISR entry (genuine register
// save/restore, genuine interrupt arbitration) under full test control,
// without needing a free-running hardware timer that would contend with
// sleep_until()'s own dual-timer use elsewhere in this same binary.

namespace {

// ARMv7-M System Control Space (SCS) NVIC registers - architectural,
// the same address on any Cortex-M3, unlike UART0/the dual-timer's own
// MMIO base addresses (platform_mps2an385.cppm), which are specific to
// this board's peripheral map.
constexpr std::uint32_t nvic_iser0 = 0xE000E100U;
constexpr std::uint32_t nvic_icer0 = 0xE000E180U;
constexpr std::uint32_t nvic_ispr0 = 0xE000E200U;
constexpr std::uint32_t gpio0_irqn = 6U; // startup.c's own vector table - "GPIO0"

[[nodiscard]] auto mmio32(std::uint32_t address) noexcept -> volatile std::uint32_t& {
  return *reinterpret_cast<volatile std::uint32_t*>(address); // NOLINT(*-reinterpret-cast)
}

constexpr int isr_test_item_count = 500;

// Written only by GPIO0_Handler below (while the interrupt is enabled),
// read only by mainline once it's disabled again - a fixed-capacity
// std::array + std::atomic<int> index, not a std::vector, matching
// spsc_ring<T>'s own "the atomic is the one deliberate FFI boundary,
// everything else stays single-threaded" shape (its own header
// comment). A std::vector's push_back() could reallocate, and heap
// (de)allocation from interrupt context is exactly the hazard this
// codebase already hit and fixed for real in estmsp's UART TX pump
// (docs/PLAN.md's ISR-deallocation-bug entry) - a fixed array sidesteps
// the question entirely instead of relying on a reserve() call never
// being exceeded. drained_count is std::atomic (not plain int) so the
// mainline polling loop below actually reloads it each iteration - the
// compiler has no reason to assume an MMIO write to an unrelated
// address (the ISPR poke) could change a plain int it doesn't see
// written anywhere in this translation unit's visible control flow.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::array<int, isr_test_item_count> drained{};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> drained_count{0};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
est::spsc_ring<int>* active_ring = nullptr;

} // namespace

// startup.c's own vector table references this by name at slot 16+6
// (IRQ6), overriding the weak default-to-Default_Handler alias it
// declares for every build that doesn't define this symbol - i.e. every
// build except this one. One try_pop() per firing, matching
// UART0_TX_Handler's own "one unit of work per interrupt" shape.
// active_ring is only ever written by the test body below, and only
// while this interrupt is disabled (before NVIC enable / after NVIC
// disable) - never concurrently with this handler, so no synchronization
// is needed on that pointer itself.
extern "C" void GPIO0_Handler() noexcept { // NOLINT(readability-identifier-naming)
  if (active_ring == nullptr) {
    return;
  }
  if (const auto item = active_ring->try_pop()) {
    const auto index = drained_count.load(std::memory_order_relaxed);
    if (std::cmp_less(index, drained.size())) {
      drained[index] = *item;
      drained_count.store(index + 1, std::memory_order_relaxed);
    }
  }
}

TEST_CASE("spsc_ring: a real ISR consumer racing the mainline producer stays correct",
          "[spsc_ring]") {
  est::spsc_ring<int> ring(8); // small on purpose - forces real full/empty contention

  drained_count.store(0, std::memory_order_relaxed);
  active_ring = &ring;

  mmio32(nvic_iser0) = 1U << gpio0_irqn; // enable - disabled again at the end of this case

  // try_push() takes T&& (its own doc comment in spsc_ring.cppm), so
  // each call needs a prvalue - not `i` itself, a named loop variable
  // and therefore an lvalue. Matches spsc_ring_thread_tests.cpp's own
  // identical helper/reasoning.
  auto as_prvalue = [](int value) { return value; };
  for (int i = 0; i < isr_test_item_count; ++i) {
    while (!ring.try_push(as_prvalue(i))) {
      // Ring momentarily full: fire the consumer for real instead of
      // spinning blind - matches the real production backpressure shape
      // (estmsp::enqueue_output()'s own retry loop).
      mmio32(nvic_ispr0) = 1U << gpio0_irqn;
    }
    // Also fire opportunistically on the happy path, not just when
    // full - this is what actually creates preemption mid-push rather
    // than only between pushes: GPIO0_Handler can (and, across
    // isr_test_item_count iterations, reliably does) run while this
    // loop is somewhere in the middle of try_push() itself.
    if (i % 3 == 0) {
      mmio32(nvic_ispr0) = 1U << gpio0_irqn;
    }
  }

  // The ISR is the only thing that ever calls try_pop() here (mirroring
  // the real production split - mainline pushes, the interrupt pops),
  // so draining the last few items needs to keep firing until it
  // actually has, not assume the last mmio32(nvic_ispr0) write above
  // already resolved by the time execution reaches here.
  while (drained_count.load(std::memory_order_relaxed) < isr_test_item_count) {
    mmio32(nvic_ispr0) = 1U << gpio0_irqn;
  }

  mmio32(nvic_icer0) = 1U << gpio0_irqn; // disable again - this case is done with it
  active_ring = nullptr;

  REQUIRE(ring.try_pop() == std::nullopt); // fully drained, nothing left behind

  std::vector<int> expected(isr_test_item_count);
  std::iota(expected.begin(), expected.end(), 0);
  const std::vector<int> actual(drained.begin(), drained.begin() + isr_test_item_count);
  REQUIRE(actual == expected);
}
