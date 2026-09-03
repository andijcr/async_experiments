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

# TODO(M0 follow-up): pin the exact CMAKE_EXPERIMENTAL_CXX_IMPORT_STD gate
# value once the Docker image's CMake version is pinned (see
# docs/PLAN.md, "Known open items" — apt.llvm.org/cmake.org were
# unreachable from the session that wrote this scaffolding). Until then,
# `import std;` support is opt-in and silently unavailable rather than a
# hard requirement, so the walking skeleton avoids it.
#
# set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "<gate-value-for-pinned-cmake-version>")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "est links statically by default" FORCE)

# Clang defaults to libstdc++ (or whatever GCC is on PATH) unless told
# otherwise, and `import std;`/<print> support needs libc++ specifically.
# The devenv Dockerfile installs libc++'s dev packages for exactly this.
# Opt-out (not a hardcoded flag) so this CMakeLists.txt still configures
# on a Clang toolchain that only has libstdc++ available, e.g. for
# structural smoke-testing outside the pinned devenv image.
option(EST_USE_LIBCXX "Link against libc++ when compiling with Clang" ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND EST_USE_LIBCXX)
  add_compile_options(-stdlib=libc++)
  add_link_options(-stdlib=libc++)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

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
