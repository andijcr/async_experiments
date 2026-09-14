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
# deliberately has no est:: dependency at all (see its own top comment).
target_compile_features(larson_scanner PUBLIC cxx_std_23)
est_set_warnings(larson_scanner)
est_enable_coverage(larson_scanner)
est_enable_sanitizers(larson_scanner)

# The est::-dependent half (larson_scanner_app.cppm's own top comment):
# the est::loop/est::schedule_periodic/est::spsc_ring<T>/
# est::external_event<T> wiring, behind an interface with no future<T> in
# it. A separate library target, not folded into larson_scanner above,
# for the identical reason spreadsheet/spreadsheet_io are two separate
# targets in examples/spreadsheet/CMakeLists.txt: one half is pure and
# est-free, the other isn't, and nothing should have to link the second
# just to use the first (larson_scanner_tests.cpp, notably, still only
# links larson_scanner::larson_scanner).
add_library(larson_scanner_app STATIC)
add_library(larson_scanner::larson_scanner_app ALIAS larson_scanner_app)

target_sources(larson_scanner_app
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS src
    FILES
      src/larson_scanner_app.cppm
)

target_link_libraries(larson_scanner_app PUBLIC est::est larson_scanner::larson_scanner)
target_compile_features(larson_scanner_app PUBLIC cxx_std_23)
est_set_warnings(larson_scanner_app)
est_enable_coverage(larson_scanner_app)
est_enable_sanitizers(larson_scanner_app)

add_executable(multicolor_larson_scanner main.cpp)
target_link_libraries(multicolor_larson_scanner
  PRIVATE est::est estext::estext larson_scanner::larson_scanner larson_scanner::larson_scanner_app)
est_set_warnings(multicolor_larson_scanner)
est_enable_coverage(multicolor_larson_scanner)
est_enable_sanitizers(multicolor_larson_scanner)

if(EST_BUILD_TESTS)
  add_subdirectory(tests)
endif()
