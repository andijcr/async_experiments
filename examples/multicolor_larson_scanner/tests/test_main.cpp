import est;
import estext;

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

// Mirrors examples/spreadsheet/tests/test_main.cpp: installs hosted_stdcpp
// once, before any TEST_CASE runs, so platform::instance() is never null -
// needed here for larson_scanner_app_tests.cpp's cases, which drive a real
// est::loop/est::schedule_periodic() the same way main.cpp does.
// larson_scanner_tests.cpp's own pure-logic cases don't need this, but
// having it installed doesn't affect them either.
auto main(int argc, char* argv[]) -> int {
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);
  return Catch::Session().run(argc, argv);
}
