import est;
import estext;

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

// Mirrors est/tests/test_main.cpp: installs hosted_stdcpp once, before any
// TEST_CASE runs, so platform::instance() is never null - needed here
// specifically for sheet_tests.cpp's resolve_blocking() cases, which
// suspend on a real est::loop/est::future the same way the real server
// does.
auto main(int argc, char* argv[]) -> int {
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);
  return Catch::Session().run(argc, argv);
}
