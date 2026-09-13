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
    // est::loop takes a std::pmr::polymorphic_allocator<std::byte>
    // (default: std::pmr::get_default_resource(), ordinarily
    // new_delete_resource()) that every future_state<T>, continuation
    // node, and coroutine resume node it ever drives is built through. A
    // caller who cares about that allocation traffic can swap in any
    // std::pmr::memory_resource with no changes to est itself -
    // std::pmr::unsynchronized_pool_resource (the single-threaded
    // variant, matching est::loop's own no-atomics constraint) measured
    // about 9-10% faster than the default new_delete_resource() on a
    // workload mixing .then() chains and suspending co_awaits (issue
    // #47, docs/wiki/Allocation-Patterns.md). Not needed for a program
    // this small - shown here purely as the opt-in pattern.
    std::pmr::unsynchronized_pool_resource pool;
    est::loop loop{&pool};
    const auto loop_guard = est::make_current_loop(loop);
    auto future = est::make_ready_future<int>(42);
    std::println("est::future value: {}", future.get());
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
