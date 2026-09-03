import est;

#include <cstdlib>
#include <print>

auto main() -> int {
  try {
    std::println("{}", est::placeholder_message());
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
