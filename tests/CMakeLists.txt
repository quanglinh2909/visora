add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE visora::core)
target_include_directories(core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME core_tests COMMAND core_tests)

add_executable(hal_tests hal_tests.cpp)
target_link_libraries(hal_tests PRIVATE visora::hal visora::backends)
target_include_directories(hal_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME hal_tests COMMAND hal_tests)
