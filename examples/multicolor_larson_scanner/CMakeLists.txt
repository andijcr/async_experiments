add_library(larson_scanner STATIC)
add_library(larson_scanner::larson_scanner ALIAS larson_scanner)

target_sources(larson_scanner
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS src
    FILES
      src/larson_scanner.cppm
)

# No target_link_libraries(... est::est) here - larson_scanner.cppm
# deliberately has no est:: dependency at all (see its own top comment):
# main.cpp is what wires it into est::loop/est::spsc_ring<T>/
# est::external_event<T>.
target_compile_features(larson_scanner PUBLIC cxx_std_23)
est_set_warnings(larson_scanner)
est_enable_coverage(larson_scanner)
est_enable_sanitizers(larson_scanner)

add_executable(multicolor_larson_scanner main.cpp)
target_link_libraries(multicolor_larson_scanner
  PRIVATE est::est estext::estext larson_scanner::larson_scanner)
est_set_warnings(multicolor_larson_scanner)
est_enable_coverage(multicolor_larson_scanner)
est_enable_sanitizers(multicolor_larson_scanner)

if(EST_BUILD_TESTS)
  add_subdirectory(tests)
endif()
