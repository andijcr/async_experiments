include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/EstSources.cmake")
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
est_set_warnings(est)
est_enable_coverage(est)
est_enable_sanitizers(est)

if(EST_BUILD_TESTS)
  add_subdirectory(tests)
endif()
