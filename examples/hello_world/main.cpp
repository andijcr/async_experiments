import est;
import std;

// EXIT_FAILURE/EXIT_SUCCESS are macros - unlike std::println below, they
// aren't transmitted by `import std;` (modules don't carry preprocessor
// state), so <cstdlib> stays a plain #include.
#include <cstdlib>

auto main() -> int {
  try {
    auto [promise, future] = est::make_promise_future<int>();
    promise.set_value(42);
    std::println("est::future value: {}", future.get());
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
