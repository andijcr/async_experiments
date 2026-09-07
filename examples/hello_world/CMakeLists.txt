add_executable(hello_world main.cpp)
target_link_libraries(hello_world PRIVATE est::est estext::estext)
est_set_warnings(hello_world)
