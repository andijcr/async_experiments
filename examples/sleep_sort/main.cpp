import est;
import estext;
import std;

// EXIT_FAILURE/EXIT_SUCCESS are macros - unlike std::println below, they
// aren't transmitted by `import std;` (modules don't carry preprocessor
// state), so <cstdlib> stays a plain #include.
#include <cstdlib>

// Sleep sort, 1 to 10: schedules one est::sleep_for(n * unit) per
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
  // A concrete platform::interface isn't installed automatically just by
  // `import est;` - that's this program's own decision to make, not
  // something the library does invisibly; est itself doesn't even know
  // estext exists. estext::hosted_stdcpp is the one that exists for a
  // hosted program like this one, and this example genuinely needs it
  // installed: run_until_idle() below calls through platform::instance()
  // for real timer waits. platform_instance/platform_guard are declared
  // first, right at the top of main(), so they outlive everything that
  // might touch it.
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  try {
    std::array<int, 10> numbers{};
    std::ranges::iota(numbers, 1);
    std::ranges::shuffle(numbers, std::mt19937{std::random_device{}()});

    std::println("input: {}", numbers);

    est::loop loop;
    const auto loop_guard = est::make_current_loop_with_spawn(loop);
    using namespace std::chrono_literals;

    constexpr auto unit = 100ms;
    for (const int n : numbers) {
      // est::spawn() (issue #58): explicit, loop-owned ownership of this
      // fire-and-forget chain instead of a discarded future<void> handle
      // relying on the timer node's own promise to keep the chain alive
      // on its own (docs/wiki/Allocation-Patterns.md).
      est::spawn(est::sleep_for(n * unit).then([n] { std::println("{}", n); }));
    }

    loop.run_until_idle();
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
