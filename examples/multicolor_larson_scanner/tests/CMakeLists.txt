include(FetchContent)

# FetchContent_Declare is idempotent across multiple calls with the same
# name in one configure run, so this is safe even though est/tests/
# CMakeLists.txt may already have declared Catch2 earlier.
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)

add_executable(larson_scanner_tests
  test_main.cpp
  larson_scanner_tests.cpp
  larson_scanner_app_tests.cpp
)
# Catch2::Catch2, not Catch2::Catch2WithMain: test_main.cpp supplies its
# own main(), installing a platform::interface before any TEST_CASE runs
# - needed for larson_scanner_app_tests.cpp's cases, which drive a real
# est::loop (larson_scanner_tests.cpp's own pure-logic cases don't need
# it, but having one installed doesn't affect them either), matching
# examples/spreadsheet/tests/CMakeLists.txt's identical reasoning.
target_link_libraries(larson_scanner_tests PRIVATE
  est::est
  estext::estext
  larson_scanner::larson_scanner
  larson_scanner::larson_scanner_app
  Catch2::Catch2
)
est_set_warnings(larson_scanner_tests)
est_enable_coverage(larson_scanner_tests)
est_enable_sanitizers(larson_scanner_tests)

# -Wc2y-extensions workaround - see est/tests/CMakeLists.txt's own
# comment on the same flags.
target_compile_options(larson_scanner_tests PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(larson_scanner_tests)
