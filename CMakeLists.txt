cmake_minimum_required(VERSION 3.20)
project(visora VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Every executable lands in build/bin, whichever subdirectory declares it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(VisoraOptions)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/hal)
add_subdirectory(src/media)
add_subdirectory(src/store)

if(VISORA_WITH_API)
    add_subdirectory(src/api)
    add_subdirectory(src/apps/visora)
endif()
add_subdirectory(src/vision)
add_subdirectory(src/apps/visora-probe)
add_subdirectory(tests)

visora_print_summary()
