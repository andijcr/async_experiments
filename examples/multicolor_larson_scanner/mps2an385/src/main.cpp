import est;
import estmsp;
import larson_scanner;
import larson_scanner_app;
import std;

// QEMU's mps2-an385 has no interactive stdin/terminal (issue #123
// Finding 11 - no button/switch input is even modeled), so unlike the
// terminal/wasm ports there is no input thread: bare-metal single-core
// has no real OS threads to run one on anyway. This program instead
// runs the animation for a fixed number of rendered frames against the
// mps2-scc peripheral's 8 real LEDs, then quits and exits via
// semihosting - a deterministic run shape suited to what this backend
// is actually for (a QEMU-driven CI smoke test), not a demo someone
// drives by hand.

namespace {

constexpr std::uint32_t scc_cfg1 = 0x4002f004U; // issue #123 Finding 11

[[nodiscard]] auto scc_led_reg() noexcept -> volatile std::uint32_t& {
  // NOLINTNEXTLINE(*-reinterpret-cast,performance-no-int-to-ptr) - a
  // fixed-address MMIO register, not an accidental int/pointer mixup.
  return *reinterpret_cast<volatile std::uint32_t*>(scc_cfg1);
}

// The SCC's 8 LEDs are plain on/off (Finding 11 - CFG1 is a bitmask, not
// a PWM/analog output), so each of the 8 pixels lights up once its
// brightest channel crosses this threshold - a binary silhouette of the
// same decaying trail the terminal/wasm ports render in full color.
constexpr float led_on_threshold = 0.3F;

[[nodiscard]] auto led_mask(const larson_scanner::led_buffer& buffer) noexcept -> std::uint32_t {
  std::uint32_t mask = 0;
  const auto red = buffer.red.intensities();
  const auto green = buffer.green.intensities();
  const auto blue = buffer.blue.intensities();
  for (std::size_t i = 0; i < buffer.width; ++i) {
    // i < buffer.width, and red/green/blue are each buffer.width-sized
    // (led_buffer's own invariant) - provably in range by construction.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const float brightest = std::max({red[i], green[i], blue[i]});
    if (brightest > led_on_threshold) {
      mask |= (1U << i);
    }
  }
  return mask;
}

} // namespace

auto main() -> int {
  estmsp::platform_mps2an385 platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  // width == 8: this board has exactly 8 real LEDs (mps2-scc, issue #123
  // Finding 11) - not a downscaled approximation of some larger strip,
  // the animation is configured for the hardware it actually drives.
  constexpr std::size_t frame_budget = 200; // bounds the QEMU run for CI
  std::size_t frames_rendered = 0;
  bool quit_requested = false;

  larson_scanner::app* app_ptr = nullptr;
  larson_scanner::app scanner_app(
      larson_scanner::app_config{.width = 8},
      [&frames_rendered, &quit_requested, &app_ptr](const larson_scanner::led_buffer& buffer) {
        scc_led_reg() = led_mask(buffer);
        // Exactly once: loop_.stop() (drain_commands(), on seeing this
        // quit command) doesn't halt est::loop::run() instantly -
        // already-scheduled timer callbacks for this pass still run
        // first, so without this guard every render after the budget is
        // reached would push another copy. push_command()'s own retry
        // loop has no backoff on this no-threads target (EST_NO_THREADS,
        // cmake/toolchain-mps2an385.cmake's own comment) - once the ring
        // fills with commands nothing is left to drain, that loop spins
        // forever.
        if (++frames_rendered >= frame_budget && !quit_requested) {
          quit_requested = true;
          app_ptr->push_command({.kind = larson_scanner::CommandKind::quit});
        }
      });
  app_ptr = &scanner_app;

  scanner_app.loop(); // returns once the frame budget above requests quit

  // A plain `return` here would fall into startup.c's own trailing
  // `for(;;){}` and never terminate QEMU (that loop exists precisely for
  // the case of main() returning with no OS to return *to* - see its own
  // comment) - std::exit() routes through picolibc's normal atexit/
  // _exit() chain, and startup.c's own _exit() is what actually calls
  // the semihosting SYS_EXIT_EXTENDED that gives this process a real,
  // checkable exit code (same reasoning src/test_main.cpp's own final
  // line already has).
  std::exit(0);
}
