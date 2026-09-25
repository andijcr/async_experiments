import est;
import estwasm;
import larson_scanner;
import larson_scanner_app;
import std;

// The one TU this build actually compiles as a plain (non-module) source
// file - its whole job is wiring larson_scanner_app::app (reused
// unmodified from the terminal example) to two JS-callable entry points,
// nothing a module interface needs to expose to another C++ TU. Mirrors
// main.cpp's own role in the terminal build: everything est-shaped stays
// inside app itself, this file only turns "the Worker called boot()" /
// "the main thread called push_command()" into calls against app's own
// plain, blocking interface. See docs/PLAN.md's "Issue #99" entry for the
// full Worker/shared-memory design this implements.

namespace {

// Set once, by boot() below (the Worker's own call, the only one that
// ever runs it) - never reassigned afterward. Read by push_command()
// (the main thread's own call, on its own separately-instantiated but
// memory-sharing copy of this module) purely as a null-check; the
// pointer target itself lives in the one shared linear memory both
// instantiations operate on, so no cross-thread synchronization is
// needed to make a Worker-side write visible to a main-thread-side read
// here (proved out by the M0.5 spike, docs/PLAN.md's Issue #99 entry).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
larson_scanner::app* g_app = nullptr; // NOLINT(misc-use-internal-linkage)

// Imported from JS: the one-time startup handshake (this module's own
// top comment / docs/PLAN.md's Issue #99 entry) - called exactly once,
// from the render_callback below, the first time it runs, handing the
// main thread the three channels' own stable intensity-buffer addresses
// (scanner_channel's own std::vector<float> data() pointer, fixed for
// the buffer's lifetime once led_buffer's constructor has run) plus the
// strip width, so the main thread's own requestAnimationFrame loop can
// read them directly out of shared memory - no wasm call needed per
// frame to paint (see that entry's own "rendering is a direct
// shared-memory read" design point).
extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming)
__attribute__((import_module("env"), import_name("js_worker_ready"))) void js_worker_ready(
    std::uint32_t red_ptr, std::uint32_t green_ptr, std::uint32_t blue_ptr, std::uint32_t width);
} // extern "C"

} // namespace

// Worker-side entry point: installs estwasm's platform backend, builds
// the app, blocks forever (est::loop::run(), via app.loop()) - the same
// shape examples/multicolor_larson_scanner/main.cpp already has, just
// with a JS-callable extern "C" boundary instead of a real main(). Only
// ever called once (the Worker's own bootstrap JS calls it a single
// time right after instantiating this module).
extern "C" void boot(std::uint32_t width, float initial_speed, float initial_decay) {
  static estwasm::platform_wasm platform;
  const auto platform_guard = est::platform::override_instance(platform);

  static bool ready_reported = false;
  static larson_scanner::app scanner_app(
      larson_scanner::app_config{
          .width = width, .initial_speed = initial_speed, .initial_decay = initial_decay},
      [](const larson_scanner::led_buffer& buffer) {
        if (!ready_reported) {
          ready_reported = true;
          js_worker_ready(static_cast<std::uint32_t>(
                              reinterpret_cast<std::uintptr_t>(buffer.red.intensities().data())),
                          static_cast<std::uint32_t>(
                              reinterpret_cast<std::uintptr_t>(buffer.green.intensities().data())),
                          static_cast<std::uint32_t>(
                              reinterpret_cast<std::uintptr_t>(buffer.blue.intensities().data())),
                          static_cast<std::uint32_t>(buffer.width));
        }
      });
  g_app = &scanner_app;

  scanner_app
      .loop(); // blocks forever - Atomics.wait, via platform_wasm::interruptible_sleep_until()
}

// Main-thread-side entry point: real C++, running on the main thread's
// own call stack (not through postMessage - see this file's own top
// comment) - reaches the Worker's own scanner_app object because g_app's
// *target* lives in the memory both instantiations share, even though
// each has its own independent g_app variable/copy of this function.
// Silently drops the call if the Worker hasn't reached boot() yet (a
// narrow startup race the JS glue already avoids by keeping sliders
// disabled until js_worker_ready() fires, but push_command() itself
// stays defensive regardless of caller discipline).
extern "C" void push_command(std::int32_t channel, std::int32_t param, float value) {
  if (g_app != nullptr) {
    g_app->push_command(
        larson_scanner::command{.kind = larson_scanner::CommandKind::set_param,
                                .channel = static_cast<larson_scanner::ChannelSelector>(channel),
                                .param = static_cast<larson_scanner::ParamKind>(param),
                                .value = value});
  }
}
