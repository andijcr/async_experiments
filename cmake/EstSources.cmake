# The canonical list of est's own CXX_MODULES source files, relative to
# est/src - included solely by est/CMakeLists.txt itself, the single
# source of truth for what belongs in the `est` target's FILE_SET. The two
# standalone cross-compile projects (examples/multicolor_larson_scanner/
# web, .../mps2an385) no longer need their own copy of this list - since
# est/CMakeLists.txt became self-contained (issue #128), they
# add_subdirectory() it directly instead of hand-declaring their own
# add_library(est STATIC).
#
# Bare paths, not prefixed with "src/": include()-ing this file only sets
# EST_CXX_MODULE_SOURCES, leaving the consumer to list(TRANSFORM ...
# PREPEND ...) its own correct absolute base path before handing the
# result to target_sources(... FILES ...).
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
