include(FetchContent)

# SYSTEM (CMake >= 3.25): marks Catch2's own include directories as
# system headers (-isystem instead of -I). Doesn't fully solve the
# -Wc2y-extensions issue below on its own (Clang still flags it - see
# that comment) but is still correct/worth keeping: it silences the many
# *other* warning classes -Wpedantic etc. would otherwise raise against
# Catch2's own internals.
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
# own main() (installing a platform::interface before any TEST_CASE runs,
# per review - see that file's own doc comment), so this binary can't
# also link Catch2's own pre-built one.
#
# estext::estext isn't linked here - test_main.cpp's own `import estext;`
# needs it, but this directory is processed (via the root CMakeLists.txt's
# add_subdirectory(est), which reaches here through EST_BUILD_TESTS below)
# before add_subdirectory(estext) ever runs, so that target doesn't exist
# yet at this point. The root CMakeLists.txt adds that link edge itself,
# once both est_tests and estext::estext exist (CMP0079 - a target this
# directory doesn't own can still be given more link dependencies from
# another directory's scope, as long as the target itself already exists
# by then).
target_link_libraries(est_tests PRIVATE est::est Catch2::Catch2)
est_set_warnings(est_tests)
est_enable_coverage(est_tests)
est_enable_sanitizers(est_tests)

# Catch2's TEST_CASE expands to code using __COUNTER__ (a long-standing,
# widely-supported extension, but not yet standard - Clang 22 added
# -Wc2y-extensions to flag it). Marking Catch2's headers SYSTEM above does
# NOT suppress this specific diagnostic in practice (confirmed against
# the real pinned toolchain in CI - Clang still attributes it to our TU),
# so silence it directly. -Wno-unknown-warning-option must come first:
# older/other Clang versions that don't know -Wc2y-extensions yet would
# otherwise turn "unknown warning option" itself into a -Werror failure,
# which would break this locally on the Clang 18 this session used for
# smoke-testing (only the exact pinned Clang 22 needs the flag itself).
target_compile_options(est_tests PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(est_tests)
