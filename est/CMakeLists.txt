add_library(est STATIC)
add_library(est::est ALIAS est)

target_sources(est
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS src
    FILES
      src/est.cppm
      src/util/scope_exit.cppm
      src/util/shared_ptr.cppm
      src/platform/platform.cppm
      src/check.cppm
      src/sync/mutex.cppm
      src/timer.cppm
      src/future.cppm
      src/promise.cppm
)

target_compile_features(est PUBLIC cxx_std_23)
est_set_warnings(est)
est_enable_coverage(est)

if(EST_BUILD_TESTS)
  add_subdirectory(tests)
endif()
