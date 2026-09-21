# The canonical list of est's own CXX_MODULES source files, relative to
# est/src - the single source of truth every CMake project that defines
# its own est target includes(), rather than maintaining separate copies
# of this file list by hand. Three do: this repo's own root build
# (est/CMakeLists.txt) and the two standalone cross-compile projects
# (examples/multicolor_larson_scanner/web, .../mps2an385) - each of the
# latter two must declare its own add_library(est STATIC) rather than
# add_subdirectory()-ing est/CMakeLists.txt itself (that file's own
# est_set_warnings()/est_enable_coverage()/est_enable_sanitizers() calls
# assume the hosted toolchain's own compiler-rt/profiling/sanitizer
# runtimes - see those two projects' own top comments for the full
# reasoning), but all three need the exact same set of module source
# files, and previously kept three hand-maintained copies of this same
# list in sync by hand.
#
# Bare paths, not prefixed with "src/" or any project's own base
# directory: include()-ing this file only sets EST_CXX_MODULE_SOURCES,
# leaving every consumer to list(TRANSFORM ... PREPEND ...) its own
# correct absolute base path before handing the result to
# target_sources(... FILES ...) - the one thing genuinely different
# between the root build (relative to its own est/ directory) and the
# two standalone projects (relative to ${EST_ROOT}/est/src further up
# the tree).
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
