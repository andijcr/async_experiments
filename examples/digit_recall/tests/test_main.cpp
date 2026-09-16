import est;
import estext;

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

// Mirrors est/tests/test_main.cpp / examples/spreadsheet/tests/test_main.cpp:
// installs hosted_stdcpp once, before any TEST_CASE runs, so
// platform::instance() is never null - individual test cases that need
// a controllable clock override it locally with a fake_platform instead
// (est/tests/loop_tests.cpp's own established pattern).
auto main(int argc, char* argv[]) -> int {
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);
  return Catch::Session().run(argc, argv);
}
