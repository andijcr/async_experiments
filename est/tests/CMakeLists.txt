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
  skeleton_tests.cpp
  platform_tests.cpp
  mutex_tests.cpp
  timer_tests.cpp
  future_tests.cpp
)
target_link_libraries(est_tests PRIVATE est::est Catch2::Catch2WithMain)
est_set_warnings(est_tests)
est_enable_coverage(est_tests)

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
