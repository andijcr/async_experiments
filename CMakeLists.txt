cmake_minimum_required(VERSION 3.28)

project(async_experiments
  DESCRIPTION "est: an educational single-threaded async framework for C++23/26"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Module dependency scanning (FILE_SET CXX_MODULES) needs Ninja.
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR
    "est requires the Ninja generator for C++ module support "
    "(got '${CMAKE_GENERATOR}'). Configure with -G Ninja."
  )
endif()
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# Compiler, standard library (libc++), and the import-std experimental
# gate are pinned in cmake/toolchain-hosted-linux.cmake, applied via the
# "default"/"ci" CMake presets (CMakePresets.json) rather than set here -
# see that file for why. Configuring without a preset (or a different
# -DCMAKE_TOOLCHAIN_FILE=) falls back to CMake's normal compiler search,
# useful for structural smoke-testing against whatever toolchain happens
# to be on hand.

# Project-level opt-in for `import std;` (the toolchain file's
# CMAKE_EXPERIMENTAL_CXX_IMPORT_STD only unlocks the feature; this is
# what actually requests it - seeds every target's CXX_MODULE_STD
# property). Every est source file uses `import std;` instead of classic
# `#include`s now (docs/PLAN.md, M2 review round 5) - a build without
# the gate set (no toolchain file, e.g. this project's own structural
# local-smoke-test builds against a pre-3.30 CMake) will fail to
# configure importing std at all, not just warn; there is no
# `#include`-based fallback path left to fall back to.
set(CMAKE_CXX_MODULE_STD ON)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "est links statically by default" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Coverage)

option(EST_BUILD_TESTS "Build est unit tests" ON)
option(EST_BUILD_EXAMPLES "Build est example apps" ON)

# Must precede add_subdirectory(est): catch_discover_tests()'s add_test()
# calls only register with CTest if testing was already enabled in an
# ancestor directory scope.
if(EST_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(est)

if(EST_BUILD_EXAMPLES)
  add_subdirectory(examples/hello_world)
endif()
