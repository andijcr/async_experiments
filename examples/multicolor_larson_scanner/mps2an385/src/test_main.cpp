import est;
import estpico;
import std;

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

// This board's own equivalent of est/tests/test_main.cpp - only this file
// differs (installing estpico::platform_mps2an385 instead of
// estext::hosted_stdcpp); every other test_*.cpp reused from est/tests/
// is genuinely backend-agnostic (just `import est;`/`import std;` plus
// Catch2 macros).

namespace {

constexpr std::uint32_t uart0_base = 0x40004000U;
constexpr std::uint32_t uart0_data = uart0_base + 0x00U;
constexpr std::uint32_t uart0_state = uart0_base + 0x04U;

[[nodiscard]] auto mmio32(std::uint32_t address) noexcept -> volatile std::uint32_t& {
  return *reinterpret_cast<volatile std::uint32_t*>(address); // NOLINT(*-reinterpret-cast)
}

// CATCH_CONFIG_NOSTDOUT (tests/CMakeLists.txt's own comment) means Catch2
// itself defines no Catch::cout()/cerr()/clog() at all - the embedding
// program is expected to supply them. This target has no real stdout
// (no OS, no stdio backend - estpico's own vprintdbg() writes through
// the identical UART0 register pair for the same reason), so this
// redirects Catch2's own console reporter output through it directly -
// separate from estpico's own private UART writer (est::platform::
// interface's own diagnostic channel) since Catch2's output is a much
// higher-volume, unrelated stream that has nothing to do with
// est::platform::printdbg().
class UartStreambuf final : public std::streambuf {
protected:
  auto overflow(int_type ch) -> int_type override {
    if (ch != traits_type::eof()) {
      while ((mmio32(uart0_state) & 0x1U) != 0U) {
      }
      mmio32(uart0_data) = static_cast<std::uint32_t>(ch);
    }
    return ch;
  }
};

UartStreambuf g_uart_streambuf;
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

// No real argc/argv on this target (bare-metal main() has no command
// line) - a fixed, single-element argv matching what Catch2's own
// Session::run() needs to at least see its own program name. Runs the
// exact same TEST_CASEs the hosted est_tests binary does - a QEMU
// process exit code of 0 means every one of them passed for real, not
// just that the firmware itself booted.
//
// A plain `return` from main() would fall into startup.c's own trailing
// `for(;;){}` and never terminate QEMU - std::exit() routes through
// picolibc's normal atexit/_exit() chain, and startup.c's own _exit()
// is what actually calls the semihosting SYS_EXIT_EXTENDED that gives
// this process a real, checkable exit code.
auto main() -> int {
  // estpico::platform_mps2an385's own constructor enables UART0 TX - it
  // runs before anything below could write through g_uart_stream.
  estpico::platform_mps2an385 platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);
  const char* argv[] = {"est_mps2an385_tests"};
  const int result = Catch::Session().run(1, argv);
  std::exit(result);
}
