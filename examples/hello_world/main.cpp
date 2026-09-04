import est;

#include <cstdlib>
#include <print>

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
