# A fully independent CMake project, not add_subdirectory()'d into the
# root build - see cmake/toolchain-wasm32.cmake's own top comment for why
# (two different compilers/target-triples in one configure/generate pass
# isn't something CMake supports, and the root CMakeLists.txt's own
# CMAKE_CXX_MODULE_STD/CMAKE_CXX_SCAN_FOR_MODULES settings assume the one
# hosted-Linux toolchain throughout). Configured via its own "wasm32"
# preset (CMakePresets.json, next to this file): `cmake --preset wasm32
# -S examples/multicolor_larson_scanner/web -B build/wasm32`.
#
# Re-declares est/larson_scanner/larson_scanner_app as their own targets
# here (FILE_SET pointing at the real, shared source files, not a copy)
# rather than add_subdirectory()-ing their real CMakeLists.txt files: those
# call est_set_warnings()/est_enable_coverage()/est_enable_sanitizers()
# (cmake/CompilerWarnings.cmake and friends), which assume the hosted
# toolchain's own compiler-rt/profiling/sanitizer runtimes - none of which
# apply to a wasm32 cross-compile.
cmake_minimum_required(VERSION 4.2)
project(larson_scanner_wasm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)

set(EST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# --- est itself, cross-compiled for wasm32-wasip1-threads - same source
# files est/CMakeLists.txt's own FILE_SET lists, just no est_set_warnings()/
# est_enable_coverage()/est_enable_sanitizers() calls here (see this file's
# own top comment).
add_library(est STATIC)
target_sources(est
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${EST_ROOT}/est/src"
    FILES
      "${EST_ROOT}/est/src/est.cppm"
      "${EST_ROOT}/est/src/util/intrusive_list.cppm"
      "${EST_ROOT}/est/src/util/scope_exit.cppm"
      "${EST_ROOT}/est/src/util/shared_ptr.cppm"
      "${EST_ROOT}/est/src/platform/platform.cppm"
      "${EST_ROOT}/est/src/check.cppm"
      "${EST_ROOT}/est/src/util/jitter.cppm"
      "${EST_ROOT}/est/src/sync/mutex.cppm"
      "${EST_ROOT}/est/src/sync/event.cppm"
      "${EST_ROOT}/est/src/sync/external_event.cppm"
      "${EST_ROOT}/est/src/sync/spsc_ring.cppm"
      "${EST_ROOT}/est/src/sync/stop_token.cppm"
      "${EST_ROOT}/est/src/timer.cppm"
      "${EST_ROOT}/est/src/loop.cppm"
      "${EST_ROOT}/est/src/util/current_loop.cppm"
      "${EST_ROOT}/est/src/timer_periodic.cppm"
      "${EST_ROOT}/est/src/future.cppm"
      "${EST_ROOT}/est/src/promise.cppm"
      "${EST_ROOT}/est/src/when_all.cppm"
      "${EST_ROOT}/est/src/when_any.cppm"
      "${EST_ROOT}/est/src/when_any_succeeds.cppm"
      "${EST_ROOT}/est/src/with_stop.cppm"
)
target_compile_features(est PUBLIC cxx_std_23)

# --- estwasm: this build's own platform::interface backend ---
add_library(estwasm STATIC)
target_sources(estwasm
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${EST_ROOT}/estwasm/src"
    FILES
      "${EST_ROOT}/estwasm/src/platform_wasm.cppm"
)
target_compile_features(estwasm PUBLIC cxx_std_23)
target_link_libraries(estwasm PUBLIC est)

# --- larson_scanner / larson_scanner_app: unmodified, reused straight
# from the terminal example's own sources (issue #99's whole point - see
# docs/PLAN.md's entry).
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

# --- the actual wasm artifact: no C/C++ main() (-mexec-model=reactor,
# cmake/toolchain-wasm32.cmake) - boot()/push_command() are the only real
# entry points, exported explicitly below since nothing calls them from
# within this module itself for the linker to keep otherwise.
add_executable(larson_scanner_wasm src/wasm_exports.cpp)
target_link_libraries(larson_scanner_wasm PRIVATE est estwasm larson_scanner larson_scanner_app)
target_link_options(larson_scanner_wasm PRIVATE
  "-Wl,--export=boot" "-Wl,--export=push_command")
