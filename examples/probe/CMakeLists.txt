add_executable(probe main.cpp)
target_link_libraries(probe PRIVATE est::est estext::estext)
est_set_warnings(probe)
