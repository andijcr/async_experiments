import est;
import estext;
import std;

// EXIT_FAILURE/EXIT_SUCCESS are macros - unlike std::println below, they
// aren't transmitted by `import std;` (modules don't carry preprocessor
// state), so <cstdlib> stays a plain #include.
#include <cstdlib>

// Not a runnable example in the usual sense - a fixed set of call sites
// for inspecting the codegen `est::current_loop()`/`.allocator()` produce
// under LTO (`cmake --preset lto`), e.g. via `objdump -d -C`. Each probe
// is `noinline` so it survives as a real call site to disassemble instead
// of being folded into its caller.

[[gnu::noinline]] auto probe_current_loop() -> est::loop* {
  return &est::current_loop();
}

[[gnu::noinline]] auto probe_loop_allocator() -> void* {
  return est::current_loop().allocator().resource();
}

[[gnu::noinline]] auto probe_platform_instance() -> est::platform::interface* {
  return &est::platform::instance();
}

[[gnu::noinline]] auto probe_make_promise_future() -> est::future<int> {
  auto [prom, fut] = est::make_promise_future<int>();
  prom.set_value(42);
  return std::move(fut);
}

auto main() -> int {
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  try {
    std::println("{}", static_cast<void*>(probe_current_loop()));
    std::println("{}", probe_loop_allocator());
    std::println("{}", static_cast<void*>(probe_platform_instance()));
    std::println("{}", probe_make_promise_future().get());
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
