import est;
import estext;
import larson_scanner;
import larson_scanner_app;
import std;

// EXIT_SUCCESS/EXIT_FAILURE and std::setvbuf()'s _IOLBF are macros/
// plain declarations `import std;` doesn't carry (same reasoning as
// examples/spreadsheet/main.cpp), so these stay classic #includes.
#include <cstdio>
#include <cstdlib>

// main() itself is now the entire "I/O" half of this program: everything
// est::-shaped (the loop, the timers, the command queue/event handoff)
// lives in larson_scanner_app::app (src/larson_scanner_app.cppm) instead -
// this file only turns command-line/stdin I/O into calls against that
// class's own plain, blocking interface (push_command()/loop(), no
// future<T> in either), and turns the led_buffer it hands back via the
// render callback into the actual printed UTF8/ANSI text.
auto main() -> int {
  // A concrete platform::interface isn't installed automatically just
  // by `import est;` - this program's own decision to make, same as
  // every other examples/*/main.cpp.
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  // stdout is fully buffered by default whenever it isn't a tty -
  // without this, a redirected/piped run would never actually show
  // the animation until the process exits. Same reasoning
  // examples/spreadsheet/main.cpp gives for the identical call.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  // Everything that can actually throw (larson_scanner::app's own
  // allocations, std::jthread construction, app::loop() itself) lives
  // inside this one try block - nothing throwing is reachable from
  // main() outside it.
  try {
    larson_scanner::app scanner_app(larson_scanner::app_config{},
                                    [](const larson_scanner::led_buffer& buffer) {
                                      std::println("{}", larson_scanner::render(buffer));
                                    });

    // The controller: a real std::jthread genuinely blocks on
    // std::getline() (fine here - it's not the one thread app::loop()
    // runs on) and hands each parsed command straight to
    // scanner_app.push_command() - est::spsc_ring<T>/
    // est::external_event<T>'s own "composition, not reuse" story
    // (docs/wiki/Loop-And-Timers.md), now entirely app's own concern
    // rather than this file's.
    std::jthread input_thread([&scanner_app] {
      std::string line;
      while (std::getline(std::cin, line)) {
        if (const auto cmd = larson_scanner::parse_command(line)) {
          scanner_app.push_command(*cmd);
        }
        // an unparseable line is silently dropped - never crash on bad input
      }
      // EOF (or a read error) also shuts the app down cleanly, via the
      // same quit command a literal "quit" line produces.
      scanner_app.push_command({.kind = larson_scanner::CommandKind::quit});
    });

    scanner_app.loop(); // blocks until input_thread pushes a quit command
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
