add_executable(visora-probe main.cpp)
target_link_libraries(visora-probe PRIVATE visora::hal visora::media visora::backends)
