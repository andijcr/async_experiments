# Real Catch2 v3.7.1, with real exceptions enabled (issue #123 Findings
# 7-9), patched at build time for the handful of things a bare-metal
# target genuinely can't provide - filesystem, a monotonic clock,
# std::random_device (../third_party/catch2-baremetal.patch's own top
# comment on each patched file has the specific reasoning). The user's
# own preference (issue #123's PR-design discussion): a patch applied
# during the build, not a hand-maintained forked copy checked into this
# repo.
#
# Deliberately doesn't run Catch2's own top-level CMakeLists.txt
# (FetchContent_MakeAvailable would normally add_subdirectory() it) -
# that file assumes a hosted build throughout (installation rules,
# CATCH_BUILD_TESTING, reproducible-build settings, ...), none of which
# apply here. SOURCE_SUBDIR pointing at a directory with no CMakeLists.txt
# makes FetchContent_MakeAvailable populate the (patched) source without
# ever add_subdirectory()-ing it - this file builds the actual Catch2
# library target itself, directly from that source, with only the config
# surface this project actually needs.
include(FetchContent)
find_package(Git REQUIRED)

FetchContent_Declare(
  Catch2Baremetal
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
  PATCH_COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths -p1
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/catch2-baremetal.patch"
  UPDATE_DISCONNECTED TRUE
  SOURCE_SUBDIR does-not-exist
)
FetchContent_MakeAvailable(Catch2Baremetal)

# Catch2's own option-default script (CMake/CatchConfigOptions.cmake) -
# reused as-is rather than reinventing its ~30 option() calls, so every
# CATCH_CONFIG_* the generated catch_user_config.hpp below reads has the
# same well-formed default Catch2's own build gives it (in particular
# CATCH_CONFIG_DEFAULT_REPORTER/CATCH_CONFIG_CONSOLE_WIDTH, which have no
# hardcoded fallback in the .in template itself - see that file).
include("${catch2baremetal_SOURCE_DIR}/CMake/CatchConfigOptions.cmake")

# The three real, permanent gaps on this target (not exceptions - real
# exceptions work, Findings 7-9, so CATCH_CONFIG_DISABLE_EXCEPTIONS stays
# OFF). FORCE: CatchConfigOptions.cmake's own option() calls just ran and
# already created these as OFF; this overrides that default.
set(CATCH_CONFIG_NO_POSIX_SIGNALS ON CACHE BOOL "" FORCE)
set(CATCH_CONFIG_NOSTDOUT ON CACHE BOOL "" FORCE)
set(CATCH_CONFIG_NO_WCHAR ON CACHE BOOL "" FORCE)

configure_file(
  "${catch2baremetal_SOURCE_DIR}/src/catch2/catch_user_config.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated-includes/catch2/catch_user_config.hpp"
)

file(GLOB_RECURSE CATCH2_BAREMETAL_SOURCES CONFIGURE_DEPENDS
  "${catch2baremetal_SOURCE_DIR}/src/catch2/*.cpp")

add_library(Catch2Baremetal STATIC ${CATCH2_BAREMETAL_SOURCES})
target_include_directories(Catch2Baremetal SYSTEM PUBLIC
  "${catch2baremetal_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated-includes")
target_compile_features(Catch2Baremetal PUBLIC cxx_std_20)
# The three CATCH_BAREMETAL_NO_* macros the patch itself introduces
# (not part of Catch2's own CATCH_CONFIG_* surface above - see the patch
# file's own top comment on each site).
target_compile_definitions(Catch2Baremetal PUBLIC
  CATCH_BAREMETAL_NO_MONOTONIC_CLOCK
  CATCH_BAREMETAL_NO_RANDOM_DEVICE
  CATCH_BAREMETAL_NO_FILESYSTEM)
# Catch2's TEST_CASE expands to code using __COUNTER__ (a long-standing,
# widely-supported extension, but not yet standard) - matching
# est/tests/CMakeLists.txt's own identical suppression.
target_compile_options(Catch2Baremetal PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

# --- est's own real test suite, reused unmodified from est/tests/ - only
# test_main.cpp differs (installs estpico::platform_mps2an385 instead of
# estext::hosted_stdcpp; see src/test_main.cpp's own top comment) -
# everything else here is genuinely backend-agnostic.
set(EST_TESTS_DIR "${EST_ROOT}/est/tests")
add_executable(est_mps2an385_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/startup.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/test_main.cpp"
  "${EST_TESTS_DIR}/platform_tests.cpp"
  "${EST_TESTS_DIR}/check_tests.cpp"
  "${EST_TESTS_DIR}/intrusive_list_tests.cpp"
  "${EST_TESTS_DIR}/mutex_tests.cpp"
  "${EST_TESTS_DIR}/event_tests.cpp"
  "${EST_TESTS_DIR}/external_event_tests.cpp"
  # spsc_ring_tests.cpp excluded: it spawns a real std::jthread producer to
  # test spsc_ring<T> under genuine concurrent access - meaningless on a
  # genuinely single-core, no-OS-threads target (EST_NO_THREADS,
  # cmake/toolchain-mps2an385.cmake's own comment - <thread> doesn't even
  # exist here), not something a workaround papers over. spsc_ring<T>
  # itself is still exercised transitively (est::spawn/est::future use it
  # internally), just not by its own dedicated concurrency test here.
  "${EST_TESTS_DIR}/timer_tests.cpp"
  "${EST_TESTS_DIR}/loop_tests.cpp"
  "${EST_TESTS_DIR}/jitter_tests.cpp"
  "${EST_TESTS_DIR}/timer_periodic_tests.cpp"
  "${EST_TESTS_DIR}/future_tests.cpp"
  "${EST_TESTS_DIR}/scope_exit_tests.cpp"
  "${EST_TESTS_DIR}/shared_ptr_tests.cpp"
  "${EST_TESTS_DIR}/when_all_tests.cpp"
  "${EST_TESTS_DIR}/when_any_tests.cpp"
  "${EST_TESTS_DIR}/when_any_succeeds_tests.cpp"
  "${EST_TESTS_DIR}/stop_token_tests.cpp"
  "${EST_TESTS_DIR}/with_stop_tests.cpp"
  "${EST_TESTS_DIR}/with_timeout_tests.cpp"
  "${EST_TESTS_DIR}/spawn_tests.cpp"
)
target_link_libraries(est_mps2an385_tests PRIVATE est estpico Catch2Baremetal)
target_link_options(est_mps2an385_tests PRIVATE
  "-T${CMAKE_CURRENT_SOURCE_DIR}/../src/link.ld")
set_target_properties(est_mps2an385_tests PROPERTIES LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/link.ld")
target_compile_options(est_mps2an385_tests PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

# Deliberately no ctest/add_test() wiring for actually running these
# under QEMU: this project builds inside the pinned devenv image (CMake
# 4.4/Ninja/the ARM toolchain), which has no reason to also carry
# qemu-system-arm - the same split ci.yml's own "wasm" job already
# established for its Node-based smoke test (built inside docker, run
# against the artifact from a plain step on the runner afterward, not
# through ctest at all). ../run_under_qemu.sh is that step here: it turns
# this board's own SYS_EXIT_EXTENDED semihosting call (startup.c's
# _exit(), issue #123 Finding 1) into a real, checkable host process exit
# code for est_mps2an385_tests and larson_scanner_mps2an385_smoke alike.
