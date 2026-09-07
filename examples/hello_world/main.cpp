import est;
import std;

// EXIT_FAILURE/EXIT_SUCCESS are macros - unlike std::println below, they
// aren't transmitted by `import std;` (modules don't carry preprocessor
// state), so <cstdlib> stays a plain #include.
#include <cstdlib>

auto main() -> int {
  // A concrete platform::interface isn't installed automatically just by
  // `import est;` (per review, that would be a library silently making a
  // decision - which backend, if any - that's this program's own to
  // make). `hosted_stdcpp` is the one that exists for a hosted program
  // like this one; `platform_instance` and `platform_guard` both need to
  // outlive everything below that might touch est::check()/est::loop's
  // own platform hooks, so they're declared first, right at the top of
  // main().
  est::platform::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  try {
    est::loop loop;
    auto [promise, future] = est::make_promise_future<int>(loop);
    promise.set_value(42);
    std::println("est::future value: {}", future.get());
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
