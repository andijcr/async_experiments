include(FetchContent)

# SYSTEM (CMake >= 3.25): marks Catch2's own include directories as
# system headers, so est_set_warnings()'s -Wpedantic -Werror (applied to
# est_tests below, since it's our own target) doesn't fail on Catch2's own
# use of __COUNTER__ inside TEST_CASE - a long-standing, widely-supported
# extension, but not yet standard, so -Wpedantic flags it. Without SYSTEM,
# Catch2's headers are added via a plain -I and our flags apply to macro
# expansions from them too, even though the extension use isn't our code.
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
)
target_link_libraries(est_tests PRIVATE est::est Catch2::Catch2WithMain)
est_set_warnings(est_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(est_tests)
