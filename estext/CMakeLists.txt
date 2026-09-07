add_library(estext STATIC)
add_library(estext::estext ALIAS estext)

target_sources(estext
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS src
    FILES
      src/hosted_stdcpp.cppm
)

target_compile_features(estext PUBLIC cxx_std_23)
# PUBLIC, not PRIVATE: hosted_stdcpp.cppm's own `import est;` means every
# consumer of estext transitively needs est's own module interface (BMI)
# available too, the same reason a public header dependency propagates in
# a non-modules library.
target_link_libraries(estext PUBLIC est::est)
est_set_warnings(estext)
est_enable_coverage(estext)
est_enable_sanitizers(estext)
