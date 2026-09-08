include(FetchContent)

# Idempotent across multiple add_subdirectory() calls with the same
# target name in one configure run (est/tests/CMakeLists.txt's own doc
# comment) - safe to declare again here even though est/tests/
# CMakeLists.txt may already have done so earlier in this same configure.
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
# not just est::check()'s null-check) - est/tests/test_main.cpp's own doc
# comment explains why this can't link Catch2's pre-built main() too.
target_link_libraries(spreadsheet_tests PRIVATE
  est::est
  estext::estext
  spreadsheet::spreadsheet
  Catch2::Catch2
)
est_set_warnings(spreadsheet_tests)
est_enable_coverage(spreadsheet_tests)
est_enable_sanitizers(spreadsheet_tests)

# Same -Wc2y-extensions workaround as est/tests/CMakeLists.txt - Catch2's
# TEST_CASE macro expands to __COUNTER__-based code Clang 22 flags.
target_compile_options(spreadsheet_tests PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(spreadsheet_tests)
