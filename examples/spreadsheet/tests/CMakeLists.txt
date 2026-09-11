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

add_executable(spreadsheet_tests
  test_main.cpp
  sheet_tests.cpp
  protocol_tests.cpp
)
# Catch2::Catch2, not Catch2::Catch2WithMain: test_main.cpp supplies its
# own main(), installing a platform::interface before any TEST_CASE runs
# (sheet_tests.cpp's resolve_blocking() cases need a real, live platform,
# not just est::check()'s null-check).
target_link_libraries(spreadsheet_tests PRIVATE
  est::est
  estext::estext
  spreadsheet::spreadsheet
  Catch2::Catch2
)
est_set_warnings(spreadsheet_tests)
est_enable_coverage(spreadsheet_tests)
est_enable_sanitizers(spreadsheet_tests)

# -Wc2y-extensions workaround - see est/tests/CMakeLists.txt's own
# comment on the same flags.
target_compile_options(spreadsheet_tests PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(spreadsheet_tests)
