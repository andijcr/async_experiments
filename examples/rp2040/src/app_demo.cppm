export module app_demo;

import est;
import estrp2040;
import std;

// A minimal demo app() proving the whole pipeline end-to-end: echoes
// every byte it reads from USB CDC straight back out. Exists purely to
// give this backend a real, buildable example the same way every other
// backend in this repo has one (matching this project's own convention -
// docs/PLAN.md's entry for this backend). A separate, future project
// wanting this framework replaces this file's own app() with its own,
// unchanged main.cpp.
export auto app() -> est::future<void> {
  // Constructed only for its constructor's side effect (starting the USB
  // stack) - read()/write() are static (Serial has no per-instance state:
  // USB CDC is inherently one hardware resource), so they're called
  // through the class name rather than this instance.
  estrp2040::Serial serial;
  std::array<std::byte, 64> buffer{};
  for (;;) {
    const auto n = co_await estrp2040::Serial::read(buffer);
    co_await estrp2040::Serial::write(std::span{buffer}.first(n));
  }
}
