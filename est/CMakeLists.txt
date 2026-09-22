# Self-contained: this file only assumes CMAKE_CXX_MODULE_STD/
# CMAKE_CXX_SCAN_FOR_MODULES/CMAKE_CXX_STANDARD are already set (every
# consumer's own project() setup has to do that anyway, module support
# isn't optional), nothing else from an enclosing project(). That's what
# lets a standalone cross-compile project (examples/.../web,
# .../mps2an385 - each its own separate CMake project, a different
# compiler/target-triple than the hosted build; see those files' own top
# comments) add_subdirectory() this directory directly instead of
# hand-declaring an `est` target from cmake/EstSources.cmake itself.
option(EST_BUILD_TESTS "Build est unit tests" ON)

if(EST_BUILD_TESTS)
  enable_testing()
endif()

# The canonical list of est's own CXX_MODULES source files - this file
# is the single source of truth for what belongs in the `est` target's
# FILE_SET, no longer shared via a separate cmake/EstSources.cmake now
# that the two standalone cross-compile projects (examples/
# multicolor_larson_scanner/web, .../mps2an385) add_subdirectory() this
# file directly instead of hand-declaring their own add_library(est
# STATIC) (issue #128).
set(EST_CXX_MODULE_SOURCES
  est.cppm
  util/intrusive_list.cppm
  util/scope_exit.cppm
  util/shared_ptr.cppm
  platform/platform.cppm
  check.cppm
  util/jitter.cppm
  sync/mutex.cppm
  sync/event.cppm
  sync/external_event.cppm
  sync/spsc_ring.cppm
  sync/stop_token.cppm
  timer.cppm
  loop.cppm
  util/current_loop.cppm
  timer_periodic.cppm
  future.cppm
  promise.cppm
  when_all.cppm
  when_any.cppm
  when_any_succeeds.cppm
  with_stop.cppm
  with_timeout.cppm
  spawn.cppm
)
list(TRANSFORM EST_CXX_MODULE_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/")

add_library(est STATIC)
add_library(est::est ALIAS est)

target_sources(est
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS src
    FILES ${EST_CXX_MODULE_SOURCES}
)

target_compile_features(est PUBLIC cxx_std_23)

# Warnings/coverage/sanitizers are opt-in, not assumed: applied only if
# the caller has already include()-d cmake/CompilerWarnings.cmake and
# friends (the root CMakeLists.txt does, for the hosted build). The
# standalone cross-compile projects above deliberately don't - those
# flags assume compiler-rt/profiling/sanitizer runtimes that don't exist
# for a wasm32 or bare-metal ARM target - so these calls are just no-ops
# there instead of a hard dependency this file can't actually satisfy.
if(COMMAND est_set_warnings)
  est_set_warnings(est)
endif()
if(COMMAND est_enable_coverage)
  est_enable_coverage(est)
endif()
if(COMMAND est_enable_sanitizers)
  est_enable_sanitizers(est)
endif()

if(EST_BUILD_TESTS)
  add_subdirectory(tests)
endif()
