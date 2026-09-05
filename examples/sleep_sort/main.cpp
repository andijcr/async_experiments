import est;
import std;

// EXIT_FAILURE/EXIT_SUCCESS are macros - unlike std::println below, they
// aren't transmitted by `import std;` (modules don't carry preprocessor
// state), so <cstdlib> stays a plain #include.
#include <cstdlib>

// Sleep sort, 1 to 10: schedules one est::sleep_for(loop, n * unit) per
// number, each followed by a then() that prints n. The numbers are shuffled
// before scheduling and printed in that shuffled (unsorted) order first, so
// the sorted output that follows is visibly not just an artifact of the
// input already being in order. Every timer is registered against the same
// est::loop up front, all "at once"; est::loop itself is what ends up
// sorting them - est::timer_queue (est:timer) is a min-heap of deadlines, so
// run_until_idle() always fires the soonest pending timer next, regardless
// of the order sleep_for() calls happened in. Numbers come out in ascending
// order purely because a smaller n has a sooner deadline, never by comparing
// two numbers directly against each other - the actual point of this
// example, not the fastest way to sort 10 integers.
auto main() -> int {
  try {
    std::array<int, 10> numbers{};
    std::ranges::iota(numbers, 1);
    std::ranges::shuffle(numbers, std::mt19937{std::random_device{}()});

    std::print("input: ");
    for (const int n : numbers) {
      std::print("{} ", n);
    }
    std::println();

    est::loop loop;
    using namespace std::chrono_literals;

    constexpr auto unit = 100ms;
    for (const int n : numbers) {
      // The returned future<void> is deliberately discarded: nothing
      // needs to observe completion beyond the print itself, and the
      // chain stays alive on its own - the timer node's own promise (not
      // this handle) is what keeps the underlying future_state alive
      // until it fires (see docs/wiki/Allocation-Patterns.md).
      est::sleep_for(loop, n * unit).then([n] { std::println("{}", n); });
    }

    loop.run_until_idle();
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
