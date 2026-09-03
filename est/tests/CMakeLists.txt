include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(est_tests
  skeleton_tests.cpp
)
target_link_libraries(est_tests PRIVATE est::est Catch2::Catch2WithMain)
est_set_warnings(est_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(est_tests)
