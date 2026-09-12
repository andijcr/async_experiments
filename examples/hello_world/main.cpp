import est;
import estext;
import std;

// EXIT_FAILURE/EXIT_SUCCESS are macros - unlike std::println below, they
// aren't transmitted by `import std;` (modules don't carry preprocessor
// state), so <cstdlib> stays a plain #include.
#include <cstdlib>

auto main() -> int {
  // A concrete platform::interface isn't installed automatically just by
  // `import est;` - which backend, if any, is this program's own
  // decision to make; est itself doesn't even know estext exists.
  // `estext::hosted_stdcpp` is the one that exists for a hosted program
  // like this one; `platform_instance` and `platform_guard` both need to
  // outlive everything below that might touch est::check()/est::loop's
  // own platform hooks, so they're declared first, right at the top of
  // main().
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  try {
    est::loop loop;
    const auto loop_guard = est::make_current_loop(loop);
    auto [promise, future] = est::make_promise_future<int>();
    promise.set_value(42);
    std::println("est::future value: {}", future.get());
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
