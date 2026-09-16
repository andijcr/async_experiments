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

add_executable(digit_recall_tests
  test_main.cpp
  digit_recall_tests.cpp
)
# Catch2::Catch2, not Catch2::Catch2WithMain: test_main.cpp supplies its
# own main(), installing a platform::interface before any TEST_CASE runs.
target_link_libraries(digit_recall_tests PRIVATE
  est::est
  estext::estext
  digit_recall::digit_recall
  Catch2::Catch2
)
est_set_warnings(digit_recall_tests)
est_enable_coverage(digit_recall_tests)
est_enable_sanitizers(digit_recall_tests)

# -Wc2y-extensions workaround - see est/tests/CMakeLists.txt's own
# comment on the same flags.
target_compile_options(digit_recall_tests PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(digit_recall_tests)
