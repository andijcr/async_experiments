module;

#include <chrono>

export module est:platform;

// The hosted-Linux platform backend - the only est::platform
// implementation that exists so far (docs/PLAN.md, "Platform
// abstraction (HAL)"). It's a stateless policy type (static members
// only) so est::basic_mutex/est::timer_queue can be templated on it and
// a future bare-metal backend, or a test fake, is a drop-in replacement
// rather than a framework change.
export namespace est::platform {

struct hosted_linux {
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  [[nodiscard]] static auto now() noexcept -> time_point { return clock::now(); }

  // Hosted Linux doesn't model interrupt/signal reentrancy yet (single
  // mainline execution is all M1 covers) - a bare-metal backend's
  // critical section masks interrupts here instead of doing nothing.
  static void enter_critical_section() noexcept {}
  static void leave_critical_section() noexcept {}
};

} // namespace est::platform
