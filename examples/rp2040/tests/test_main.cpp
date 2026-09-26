#include "hardware/uart.h"

import est;
import estrp2040;
import std;

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

// This board's own equivalent of est/tests/test_main.cpp - only this file
// differs (installing estrp2040::platform_rp2040 instead of
// estext::hosted_stdcpp); every other test_*.cpp reused from est/tests/
// is genuinely backend-agnostic. Same split as mps2an385/src/
// test_main.cpp's own identical reasoning: Catch2's own console output is
// a separate, much higher-volume stream from est::platform::printdbg()'s
// own diagnostic channel, so it gets its own direct UART0 writer here
// rather than going through vprintdbg()/est::spawn() at all.
//
// Busy-wait via pico-sdk's own uart_is_writable()/uart_putc_raw(), not
// platform_rp2040.cppm's own interrupt-driven TX ring: this runs before
// (and regardless of whether) an est::loop ever starts, so there's no
// pump task to rely on - identical reasoning to mps2an385's raw-MMIO
// uart_streambuf, just through pico-sdk's own API instead of hand-rolled
// register offsets (this target already links pico-sdk, so there's no
// reason to duplicate its PL011 knowledge).
namespace {

class uart_streambuf final : public std::streambuf {
protected:
  auto overflow(int_type ch) -> int_type override {
    if (ch != traits_type::eof()) {
      while (!uart_is_writable(uart0)) {
      }
      uart_putc_raw(uart0, static_cast<char>(ch));
    }
    return ch;
  }
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
uart_streambuf g_uart_streambuf;
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
std::ostream g_uart_stream(&g_uart_streambuf);

} // namespace

namespace Catch {
auto cout() -> std::ostream& {
  return g_uart_stream;
}
auto cerr() -> std::ostream& {
  return g_uart_stream;
}
auto clog() -> std::ostream& {
  return g_uart_stream;
}
} // namespace Catch

// No real process/exit-code concept on this target (platform_rp2040.cppm's
// own top comment: no semihosting/debug-host path) - unlike mps2an385,
// which turns Catch2's result into a real QEMU process exit code, this
// prints an explicit sentinel line over the same UART0 stream instead
// and parks forever. Renode's own Robot Framework "Wait For Line On
// Uart" keyword (docs/PLAN.md's own entry for this spike) reads that
// line directly, no semihosting/exit-code plumbing needed at all.
auto main() -> int {
  estrp2040::platform_rp2040 platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);
  const std::array<const char*, 1> argv{"est_rp2040_tests"};
  const int result = Catch::Session().run(1, argv.data());
  g_uart_stream << "RP2040_TESTS_EXIT:" << result << "\n";
  g_uart_stream.flush();
  for (;;) {
    __asm__ volatile("wfi");
  }
}
