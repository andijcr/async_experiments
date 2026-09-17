add_library(digit_recall STATIC)
add_library(digit_recall::digit_recall ALIAS digit_recall)

target_sources(digit_recall
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS src
    FILES
      src/digit_recall.cppm
      src/digit_recall_io.cppm
)

target_compile_features(digit_recall PUBLIC cxx_std_23)
target_link_libraries(digit_recall PUBLIC est::est)
est_set_warnings(digit_recall)
est_enable_coverage(digit_recall)
est_enable_sanitizers(digit_recall)

add_executable(digit_recall_game main.cpp)
target_link_libraries(digit_recall_game PRIVATE est::est estext::estext digit_recall::digit_recall)
est_set_warnings(digit_recall_game)
est_enable_coverage(digit_recall_game)
est_enable_sanitizers(digit_recall_game)

if(EST_BUILD_TESTS)
  add_subdirectory(tests)
endif()
