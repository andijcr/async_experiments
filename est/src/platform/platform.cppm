export module est:platform;

import std;

// The hosted-Linux platform backend - the only est::platform
// implementation that exists so far (docs/PLAN.md, "Platform
// abstraction (HAL)"). It's a stateless policy type (static members
// only) so est::timer_queue can be templated on it and a future
// bare-metal backend, or a test fake, is a drop-in replacement rather
// than a framework change.
export namespace est::platform {

struct hosted_linux {
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  [[nodiscard]] static auto now() noexcept -> time_point { return clock::now(); }

  // Reports a failed est::check() and terminates - the hosted-Linux
  // backend's answer to "what actually happens when a check fails,"
  // same reasoning as now() being the backend's answer to "what time
  // is it": a future bare-metal backend, or a test fake, gets to
  // answer this differently (halt, trigger a debug break, ...) without
  // est::check() itself changing.
  [[noreturn]] static void assert_failure(std::string_view message,
                                          std::source_location location) noexcept {
    std::cerr << location.file_name() << ':' << location.line() << ": assertion failed";
    if (!message.empty()) {
      std::cerr << ": " << message;
    }
    std::cerr << " (in " << location.function_name() << ")\n";
    std::abort();
  }
};

} // namespace est::platform
