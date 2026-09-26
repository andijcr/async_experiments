module;

#include "tusb.h"

export module estrp2040:serial;

import est;
import std;

// estrp2040::Serial: an Arduino-`Serial`-like USB CDC class with a
// future-returning interface, backed by pico-sdk's own vendored TinyUSB
// (`lib/tinyusb`, MIT) rather than a hardware UART - docs/PLAN.md's own
// entry for this backend has the full "why USB CDC, not UART" context
// (platform_rp2040.cppm's own UART0 stays reserved for platform
// diagnostics only, never exposed as a second user-facing Serial).
//
// Architecturally simpler than platform_rp2040.cppm's own async UART TX
// driver: TinyUSB owns the real USB core interrupt itself (registered
// internally by tud_init(), never touched by this module) and does all
// protocol handling from tud_task(), which this module drives via
// est::schedule_periodic() rather than a hand-written ISR - so
// tud_cdc_rx_cb()/tud_cdc_tx_complete_cb() below always run from
// mainline (inside that periodic callback's own call stack), never real
// interrupt context. No interrupt_guard, no separate ISR-safe ring
// buffer: TinyUSB's own internal CDC FIFOs (sized by tusb_config.h's
// CFG_TUD_CDC_RX_BUFSIZE/TX_BUFSIZE, supplied by whatever links this in)
// are the only buffering this class needs on top of.
//
// USB device/configuration/string descriptors are deliberately *not*
// part of this module: those are product-specific (VID/PID, product
// strings) in a way a reusable platform backend shouldn't hardcode - the
// consuming project supplies its own tud_descriptor_*_cb() definitions
// (examples/rp2040/src/usb_descriptors.c has this project's own demo
// ones), the same split startup.c/link.ld have from estmsp in the
// mps2an385 backend (the platform module owns the mechanism, the
// application image owns the product-specific specifics).
namespace estrp2040::detail {

constexpr std::uint8_t cdc_itf = 0;

// Set from tud_cdc_rx_cb() (always mainline, this module's own top
// comment) whenever new bytes have landed in TinyUSB's own internal RX
// FIFO - read() below awaits this instead of polling tud_cdc_available().
// automatic reset mode + binary: exactly the same "wake up and check for
// work" shape platform_rp2040.cppm's own pump_wake_event already
// documents, not a counted resource.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline est::binary_event<est::EventResetMode::automatic> rx_available_event;

// Set from tud_cdc_tx_complete_cb() whenever TinyUSB's own internal TX
// FIFO has drained enough for a previously-full tud_cdc_write() to
// succeed - write() below awaits this when it needs to retry.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline est::binary_event<est::EventResetMode::automatic> tx_space_event;

// TinyUSB's own device stack needs regular polling from mainline (no
// separate "USB protocol" interrupt of its own to hang a callback off)
// - 1ms matches TinyUSB's own documented recommendation for tud_task()
// polling latency. Held for the process lifetime (never cancelled):
// nothing in this codebase tears down a USB device instance once
// started, the same "runs until the board resets" assumption every other
// platform-owned periodic/interrupt facility in this codebase already
// makes.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline std::optional<est::periodic_timer_handle> tud_task_handle;

void ensure_usb_started() {
  if (tud_task_handle) {
    return;
  }
  tusb_init();
  tud_task_handle = est::schedule_periodic(std::chrono::milliseconds(1), [] { tud_task(); });
}

} // namespace estrp2040::detail

// TinyUSB's own weak defaults (TU_ATTR_WEAK, tusb/class/cdc/cdc_device.h)
// - this module's strong definitions override them, extern "C" linkage
// since TinyUSB calls these by fixed name. Both always run from mainline
// (this module's own top comment) - safe to touch est::binary_event
// directly, no interrupt_guard needed.
extern "C" void tud_cdc_rx_cb(std::uint8_t itf) { // NOLINT(readability-identifier-naming)
  if (itf == estrp2040::detail::cdc_itf) {
    estrp2040::detail::rx_available_event.set();
  }
}

extern "C" void tud_cdc_tx_complete_cb(std::uint8_t itf) { // NOLINT(readability-identifier-naming)
  if (itf == estrp2040::detail::cdc_itf) {
    estrp2040::detail::tx_space_event.set();
  }
}

export namespace estrp2040 {

// The Arduino-`Serial`-like class itself. Starts TinyUSB's own device
// stack (idempotently - safe to construct more than one Serial, though
// nothing in this codebase does) on first use rather than at module load,
// matching platform_rp2040.cppm's own "defer to first use, once a loop
// is definitely available" pattern for its own lazily-constructed
// globals - est::schedule_periodic() itself needs est::current_loop() to
// resolve, so this can't happen any earlier than a Serial object's own
// first real use anyway.
// Capitalized to mirror Arduino's own `Serial` naming, the whole point
// of this class existing.
// NOLINTNEXTLINE(readability-identifier-naming)
class Serial {
public:
  Serial() { detail::ensure_usb_started(); }

  // Awaits until at least one byte is available, then copies as many
  // bytes as fit in `buffer` (up to `buffer.size()`) - a partial read is
  // a normal, expected outcome (matching e.g. POSIX read()'s own
  // contract), not something a caller needs to loop past unless it
  // specifically wants exactly buffer.size() bytes. A `static` member
  // rather than a free function: USB CDC is inherently one hardware
  // resource (one port), so there's no real per-instance state to speak
  // of, but calling this through a `Serial` object still reads like
  // Arduino's own `Serial.read()`.
  [[nodiscard]] static auto read(std::span<std::byte> buffer) -> est::future<std::size_t> {
    while (tud_cdc_available() == 0) {
      co_await detail::rx_available_event.wait();
    }
    const auto n = tud_cdc_read(buffer.data(), static_cast<std::uint32_t>(buffer.size()));
    co_return static_cast<std::size_t>(n);
  }

  // Writes every byte of `data`, awaiting TinyUSB's own internal TX FIFO
  // whenever it's too full to accept the rest right now, then flushes -
  // unlike read(), this always writes the whole span (matching Arduino's
  // own Serial.write() contract of "blocks/waits until it's all queued",
  // just suspending this coroutine instead of a real hardware wait).
  [[nodiscard]] static auto write(std::span<const std::byte> data) -> est::future<void> {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const auto remaining = data.subspan(offset);
      const auto n = tud_cdc_write(remaining.data(), static_cast<std::uint32_t>(remaining.size()));
      offset += n;
      if (offset < data.size()) {
        co_await detail::tx_space_event.wait();
      }
    }
    tud_cdc_write_flush();
  }
};

} // namespace estrp2040
