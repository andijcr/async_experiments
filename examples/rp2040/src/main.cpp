import est;
import estrp2040;
import app_demo;
import std;

// The reusable "framework main()": installs the RP2040 platform backend,
// constructs an est::loop, calls the project-supplied `app()`
// (app_demo.cppm's own definition here; a separate, future project
// replaces just that one file, unchanged everywhere else) and drives it
// forever - docs/PLAN.md's own entry for this backend has the full
// design context. No pico-sdk setup of its own belongs here: pico-sdk's
// own runtime_init()/clocks_init() already ran, automatically, via its
// own crt0 before main() was ever called.
//
// Unlike estmsp's own main.cpp (examples/multicolor_larson_scanner/
// mps2an385/src/main.cpp), this never returns or calls std::exit(): real
// RP2040 hardware has no host to report an exit code to, and
// loop.run()'s own "wait for external work" contract keeps this running
// forever once app()'s USB CDC polling timer is scheduled - the same
// "runs until the board resets" assumption every other bare-metal
// platform-owned facility in this codebase already makes.
// app()'s coroutine frame is allocated the moment it's called (before its
// first suspension point), so a failure there (e.g. bad_alloc) can escape
// this call before app() ever returns a future to attach a continuation
// to - on this target, unrecoverable regardless, so terminating via the
// noexcept violation is the correct fail-fast outcome, not a bug to route
// around (same reasoning as platform_rp2040.cppm's own
// bugprone-exception-escape suppressions).
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
  estrp2040::platform_rp2040 platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);

  auto app_future = app();
  loop.run();

  return 0;
}
