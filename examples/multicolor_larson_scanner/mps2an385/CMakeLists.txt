# A fully independent CMake project, not add_subdirectory()'d into the
# root build - see cmake/toolchain-mps2an385.cmake's own top comment for
# why (the same reasoning web/CMakeLists.txt gives for its own wasm32
# cross-compile). Configured via its own "mps2an385" preset
# (CMakePresets.json, next to this file): `cmake --preset mps2an385
# -DMPS2AN385_TOOLCHAIN_ROOT=/path/to/LLVM-ET-Arm-<version>-Linux-x86_64
# -S examples/multicolor_larson_scanner/mps2an385 -B build/mps2an385`.
#
# est/CMakeLists.txt is self-contained (issue #128) and gets
# add_subdirectory()'d directly below. larson_scanner/larson_scanner_app
# still get re-declared here (FILE_SET pointing at the real, shared source
# files, not a copy) rather than add_subdirectory()-ing their real
# CMakeLists.txt - see web/CMakeLists.txt's own identical comment for why.
cmake_minimum_required(VERSION 4.4)
project(larson_scanner_mps2an385 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)

set(EST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# --- est itself, cross-compiled for armv7m-none-eabi - the real
# est/CMakeLists.txt, add_subdirectory()'d directly (issue #128): its own
# est_set_warnings()/est_enable_coverage()/est_enable_sanitizers() calls
# are no-ops unless a caller has already include()-d
# cmake/CompilerWarnings.cmake and friends, which this project
# deliberately doesn't (see this file's own top comment). EST_BUILD_TESTS
# OFF: est/tests/CMakeLists.txt assumes the hosted toolchain (FetchContent
# for Catch2, real filesystem/threads), not this cross-compile - this
# project's own tests/ (added below) reuses est/tests/'s source files
# directly instead.
set(EST_BUILD_TESTS OFF)
add_subdirectory("${EST_ROOT}/est" "${CMAKE_CURRENT_BINARY_DIR}/est")

# --- estpico: this build's own platform::interface backend ---
add_library(estpico STATIC)
target_sources(estpico
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${EST_ROOT}/estpico/src"
    FILES
      "${EST_ROOT}/estpico/src/platform_mps2an385.cppm"
)
target_compile_features(estpico PUBLIC cxx_std_23)
target_link_libraries(estpico PUBLIC est)

# --- larson_scanner / larson_scanner_app: unmodified, reused straight
# from the terminal example's own sources - matching web/CMakeLists.txt's
# identical reuse.
add_library(larson_scanner STATIC)
target_sources(larson_scanner
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../src/larson_scanner.cppm"
)
target_compile_features(larson_scanner PUBLIC cxx_std_23)

add_library(larson_scanner_app STATIC)
target_sources(larson_scanner_app
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../src/larson_scanner_app.cppm"
)
target_compile_features(larson_scanner_app PUBLIC cxx_std_23)
target_link_libraries(larson_scanner_app PUBLIC est larson_scanner)

# --- the actual firmware image: this board's own Reset_Handler/vector
# table (src/startup.c) plus src/link.ld (INCLUDE picolibcpp.ld - see
# that file's own top comment) instead of any hosted CRT.
add_executable(larson_scanner_mps2an385 src/startup.c src/main.cpp)
target_link_libraries(larson_scanner_mps2an385 PRIVATE est estpico larson_scanner larson_scanner_app)
target_link_options(larson_scanner_mps2an385 PRIVATE
  "-T${CMAKE_CURRENT_SOURCE_DIR}/src/link.ld")
set_target_properties(larson_scanner_mps2an385 PROPERTIES LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/link.ld")

add_subdirectory(tests)
