import est;
import estext;

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

// Every other file in this binary just `import est;`/`import std;` and
// use Catch2's TEST_CASE macro - none of them install a
// platform::interface themselves, since that isn't an individual test
// file's job (per review: `import est;` doesn't install one either, and
// est itself doesn't even know estext/hosted_stdcpp exist - exactly like
// examples/hello_world/main.cpp and examples/sleep_sort/main.cpp each do
// their own installation, from estext, in their own main()). This file is
// the test binary's equivalent of that main() - linked against plain
// Catch2::Catch2 (not Catch2::Catch2WithMain, est/tests/CMakeLists.txt)
// so this can supply its own, installing hosted_stdcpp exactly once
// before any TEST_CASE runs.
//
// A test that needs a *different* backend still uses
// est::platform::override_instance() to retarget it for its own scope,
// same as always (est/tests/platform_tests.cpp, loop_tests.cpp,
// timer_tests.cpp all do) - this just supplies the one installed before
// that ever happens, so platform::instance() is never null to begin
// with. platform_instance/platform_guard both need to outlive the entire
// Catch2 run, hence living in main()'s own scope rather than anywhere
// narrower.
auto main(int argc, char* argv[]) -> int {
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);
  return Catch::Session().run(argc, argv);
}
