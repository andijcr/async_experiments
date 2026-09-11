include(FetchContent)

# SYSTEM (CMake >= 3.25): marks Catch2's own include directories as
# system headers (-isystem instead of -I), silencing -Wpedantic and
# friends against Catch2's own internals. Doesn't cover -Wc2y-extensions
# below, which needs its own suppression.
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)

add_executable(est_tests
  test_main.cpp
  platform_tests.cpp
  check_tests.cpp
  intrusive_list_tests.cpp
  mutex_tests.cpp
  event_tests.cpp
  timer_tests.cpp
  loop_tests.cpp
  future_tests.cpp
  scope_exit_tests.cpp
  shared_ptr_tests.cpp
)
# Catch2::Catch2, not Catch2::Catch2WithMain: test_main.cpp supplies its
# own main() (installing a platform::interface before any TEST_CASE
# runs - see that file's own doc comment), so this binary can't also
# link Catch2's own pre-built one.
#
# estext::estext isn't linked here - test_main.cpp's own `import estext;`
# needs it, but this directory is processed (via the root CMakeLists.txt's
# add_subdirectory(est)) before add_subdirectory(estext) ever runs, so
# that target doesn't exist yet at this point. The root CMakeLists.txt
# adds that link edge itself, once both est_tests and estext::estext
# exist (CMP0079 lets another directory's scope add link dependencies to
# a target it doesn't own).
target_link_libraries(est_tests PRIVATE est::est Catch2::Catch2)
est_set_warnings(est_tests)
est_enable_coverage(est_tests)
est_enable_sanitizers(est_tests)

# Catch2's TEST_CASE expands to code using __COUNTER__ (a long-standing,
# widely-supported extension, but not yet standard - Clang 22 flags it
# via -Wc2y-extensions). Marking Catch2's headers SYSTEM above does not
# suppress this diagnostic, so silence it directly.
# -Wno-unknown-warning-option must come first: an older/other Clang that
# doesn't know -Wc2y-extensions yet would otherwise turn "unknown warning
# option" itself into a -Werror failure.
target_compile_options(est_tests PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(est_tests)
