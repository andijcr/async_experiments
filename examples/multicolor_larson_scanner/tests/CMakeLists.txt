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
  larson_scanner_tests.cpp
)
# Catch2::Catch2WithMain, not the bare Catch2::Catch2 est/tests/ and
# examples/spreadsheet/tests/ use: larson_scanner.cppm has no est::
# dependency at all, so there's no platform::interface to install
# before any TEST_CASE runs.
target_link_libraries(larson_scanner_tests PRIVATE
  larson_scanner::larson_scanner
  Catch2::Catch2WithMain
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
