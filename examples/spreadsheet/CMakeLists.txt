add_library(spreadsheet STATIC)
add_library(spreadsheet::spreadsheet ALIAS spreadsheet)

target_sources(spreadsheet
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS src
    FILES
      src/spreadsheet.cppm
      src/spreadsheet_io.cppm
)

target_compile_features(spreadsheet PUBLIC cxx_std_23)
target_link_libraries(spreadsheet PUBLIC est::est)
est_set_warnings(spreadsheet)
est_enable_coverage(spreadsheet)
est_enable_sanitizers(spreadsheet)

add_executable(spreadsheet_server main.cpp)
target_link_libraries(spreadsheet_server PRIVATE est::est estext::estext spreadsheet::spreadsheet)
est_set_warnings(spreadsheet_server)
est_enable_coverage(spreadsheet_server)
est_enable_sanitizers(spreadsheet_server)

if(EST_BUILD_TESTS)
  add_subdirectory(tests)
endif()
