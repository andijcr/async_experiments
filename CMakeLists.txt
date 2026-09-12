cmake_minimum_required(VERSION 3.28)

# CMAKE_CXX_STANDARD/_STANDARD_REQUIRED/_EXTENSIONS must be set before
# project(), not after: project()'s own compiler-detection step compiles
# the `import std;` support library (the internal `__cmake_cxx23`
# target/`std.pcm`) using whatever extensions setting is active *at that
# point*, not whatever a later set() call says. Setting
# CMAKE_CXX_EXTENSIONS OFF only after project() leaves std.pcm compiled
# with GNU extensions while every est/ target compiles without them -
# Clang then refuses to load the mismatched precompiled module.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(async_experiments
  DESCRIPTION "est: an educational single-threaded async framework for C++23/26"
  LANGUAGES CXX
)

# Module dependency scanning (FILE_SET CXX_MODULES) needs Ninja.
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR
    "est requires the Ninja generator for C++ module support "
    "(got '${CMAKE_GENERATOR}'). Configure with -G Ninja."
  )
endif()
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# Project-level opt-in for `import std;` (the toolchain file's
# CMAKE_EXPERIMENTAL_CXX_IMPORT_STD only unlocks the feature; this is
# what actually requests it - seeds every target's CXX_MODULE_STD
# property). Every est source file uses `import std;` instead of classic
# `#include`s - a build without the gate set (no toolchain file, or a
# pre-3.30 CMake) fails to configure outright; there is no `#include`
# fallback path.
set(CMAKE_CXX_MODULE_STD ON)

# Compiler, standard library (libc++), and the import-std experimental
# gate are pinned in cmake/toolchain-hosted-linux.cmake, applied via the
# "default"/"ci" CMake presets (CMakePresets.json) rather than set here -
# see that file for why. Configuring without a preset (or a different
# -DCMAKE_TOOLCHAIN_FILE=) falls back to CMake's normal compiler search,
# useful for structural smoke-testing against whatever toolchain happens
# to be on hand.

set(BUILD_SHARED_LIBS OFF CACHE BOOL "est links statically by default" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Coverage)
include(Sanitizers)

option(EST_BUILD_TESTS "Build est unit tests" ON)
option(EST_BUILD_EXAMPLES "Build est example apps" ON)

# Must precede add_subdirectory(est): catch_discover_tests()'s add_test()
# calls only register with CTest if testing was already enabled in an
# ancestor directory scope.
if(EST_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(est)
add_subdirectory(estext)

# estext depends on est::est (its own CMakeLists.txt), so it has to be
# added after add_subdirectory(est) above - but est_tests is defined
# *inside* that same add_subdirectory(est) call (est/CMakeLists.txt's own
# add_subdirectory(tests)), before estext::estext exists yet. This link
# edge - test_main.cpp's own `import estext;` - has to be added here
# instead, once both targets exist (CMP0079, default NEW as of this
# project's cmake_minimum_required), rather than in est/tests/
# CMakeLists.txt itself (see that file's own comment on why).
if(EST_BUILD_TESTS)
  target_link_libraries(est_tests PRIVATE estext::estext)
endif()

if(EST_BUILD_EXAMPLES)
  add_subdirectory(examples/hello_world)
  add_subdirectory(examples/sleep_sort)
  add_subdirectory(examples/spreadsheet)
  add_subdirectory(examples/probe)
endif()
