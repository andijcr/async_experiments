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

// Added for the current-loop-only-experiment branch: probe_make_promise_future()
// above only shows current_loop() reached *once* per call. est::mutex/
// est::counting_event/a coroutine's own promise_type now each resolve it
// independently, per method, instead of caching it once at construction -
// these two probes show what that repeated resolution actually compiles to.

// Uncontended fast path only - a fresh, never-locked mutex.
[[gnu::noinline]] auto probe_mutex_lock() -> est::future<void> {
  est::mutex m;
  return m.lock();
}

// No queued waiter - the branch that never touches current_loop() at all,
// for contrast with lock() above (which always does, even uncontended).
[[gnu::noinline]] void probe_mutex_unlock_uncontended() {
  est::mutex m;
  m.unlock();
}

// A real coroutine (not make_promise_future() called directly). Never
// actually suspends (no co_await), so clang's coroutine-frame elision
// (HALO) proves the heap allocation can be skipped entirely - this probe
// exercises promise_type's constructor (current_loop() once, to build
// state_) but *not* its operator new/delete: with elision, there's no
// heap frame to allocate or free in the first place.
[[gnu::noinline]] auto probe_coroutine() -> est::future<int> {
  co_return 42;
}

// Genuinely suspends on a not-yet-ready future, which HALO can't see
// through (the frame's lifetime crosses the co_await, so eliding the heap
// allocation isn't provably safe) - this is the probe that actually
// exercises coroutine_frame_alloc()/coroutine_frame_dealloc(), each
// resolving current_loop() independently rather than sharing one lookup.
[[gnu::noinline]] auto probe_coroutine_suspending(est::future<int>& fut) -> est::future<int> {
  const int value = co_await fut;
  co_return value + 1;
}

// Prototype only - not wired into est itself, purely to get a real,
// apples-to-apples disassembly of a thread_local-based replacement for the
// platform::interface-virtual-dispatch current_loop()/allocator()/
// platform::instance() mechanism above: one thread_local context struct
// holding raw pointers to the loop, its allocator's memory_resource, and
// the active platform::interface, set by a single scoped setup function
// instead of two separate runtime-registration calls
// (make_current_loop() + platform::override_instance()).
namespace probe_tls {

struct execution_context {
  est::loop* loop_ptr = nullptr;
  std::pmr::memory_resource* resource_ptr = nullptr;
  est::platform::interface* platform_ptr = nullptr;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline thread_local execution_context tls_context;

[[nodiscard, gnu::noinline]] auto setup(est::loop& loop_ref,
                                        est::platform::interface& platform_ref) noexcept {
  tls_context = {.loop_ptr = &loop_ref,
                 .resource_ptr = loop_ref.allocator().resource(),
                 .platform_ptr = &platform_ref};
  return est::scope_exit([]() noexcept { tls_context = {}; });
}

[[nodiscard]] inline auto current_loop() noexcept -> est::loop& { return *tls_context.loop_ptr; }

[[nodiscard]] inline auto current_allocator() noexcept -> std::pmr::polymorphic_allocator<std::byte> {
  return std::pmr::polymorphic_allocator<std::byte>(tls_context.resource_ptr);
}

[[nodiscard]] inline auto current_platform() noexcept -> est::platform::interface& {
  return *tls_context.platform_ptr;
}

} // namespace probe_tls

[[gnu::noinline]] auto probe_tls_current_loop() -> est::loop* { return &probe_tls::current_loop(); }

[[gnu::noinline]] auto probe_tls_allocator() -> void* {
  return probe_tls::current_allocator().resource();
}

[[gnu::noinline]] auto probe_tls_platform_instance() -> est::platform::interface* {
  return &probe_tls::current_platform();
}

// The real mechanism's own equivalent setup cost, isolated the same way -
// today's two separate runtime-registration calls, noinline'd so they
// survive as real call sites instead of folding into main().
[[gnu::noinline]] auto probe_make_current_loop(est::loop& loop_ref) noexcept {
  return est::make_current_loop(loop_ref);
}

[[gnu::noinline]] auto probe_override_instance(est::platform::interface& platform_ref) noexcept {
  return est::platform::override_instance(platform_ref);
}

auto main() -> int {
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  try {
    std::println("{}", static_cast<void*>(probe_current_loop()));
    std::println("{}", probe_loop_allocator());
    std::println("{}", static_cast<void*>(probe_platform_instance()));
    std::println("{}", probe_make_promise_future().get());
    probe_mutex_lock().get();
    probe_mutex_unlock_uncontended();
    std::println("{}", probe_coroutine().get());

    auto [prom, fut] = est::make_promise_future<int>();
    auto suspended = probe_coroutine_suspending(fut); // suspends - fut isn't ready yet
    prom.set_value(41);
    est::current_loop().run_until_idle(); // resumes it, frame freed
    std::println("{}", suspended.get());

    est::loop mcl_loop;
    { const auto guard = probe_make_current_loop(mcl_loop); }
    { const auto guard = probe_override_instance(platform_instance); }

    est::loop tls_loop;
    const auto tls_guard = probe_tls::setup(tls_loop, platform_instance);
    std::println("{}", static_cast<void*>(probe_tls_current_loop()));
    std::println("{}", probe_tls_allocator());
    std::println("{}", static_cast<void*>(probe_tls_platform_instance()));
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
