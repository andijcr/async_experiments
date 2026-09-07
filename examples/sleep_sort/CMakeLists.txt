add_executable(sleep_sort main.cpp)
target_link_libraries(sleep_sort PRIVATE est::est estext::estext)
est_set_warnings(sleep_sort)
