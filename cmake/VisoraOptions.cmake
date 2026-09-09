# Visora build options and hardware capability detection.
#
# Nothing here fails the configure step when an accelerator is missing: every
# backend is optional and the software path is always built. What a machine
# lacks shows up in the summary table, not as an error.

include(CMakePrintHelpers)

# --- always-required software dependencies -----------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)

# --- Rockchip: RGA 2D engine + RKNN NPU runtime -------------------------------
find_path(VISORA_RGA_INCLUDE_DIR rga/im2d.h)
find_library(VISORA_RGA_LIB rga)
find_path(VISORA_RKNN_INCLUDE_DIR rknn_api.h
    HINTS "${CMAKE_SOURCE_DIR}/third_party/rknpu2/include")
find_library(VISORA_RKNN_LIB rknnrt)

if(VISORA_RGA_INCLUDE_DIR AND VISORA_RGA_LIB
        AND VISORA_RKNN_INCLUDE_DIR AND VISORA_RKNN_LIB)
    set(_visora_rockchip_default ON)
else()
    set(_visora_rockchip_default OFF)
endif()

option(VISORA_WITH_ROCKCHIP "Rockchip RGA + RKNN backends" ${_visora_rockchip_default})

# --- summary ------------------------------------------------------------------
# Collected as we go, printed once at the end of the top-level CMakeLists so the
# table is the last thing a developer sees after configuring.
set_property(GLOBAL PROPERTY VISORA_SUMMARY_LINES "")

function(visora_summary name state detail)
    get_property(_lines GLOBAL PROPERTY VISORA_SUMMARY_LINES)
    string(LENGTH "${name}" _n)
    math(EXPR _pad "20 - ${_n}")
    if(_pad LESS 1)
        set(_pad 1)
    endif()
    string(REPEAT "." ${_pad} _dots)
    list(APPEND _lines "  ${name} ${_dots} ${state}  ${detail}")
    set_property(GLOBAL PROPERTY VISORA_SUMMARY_LINES "${_lines}")
endfunction()

function(visora_print_summary)
    get_property(_lines GLOBAL PROPERTY VISORA_SUMMARY_LINES)
    message(STATUS "")
    message(STATUS "Visora ${PROJECT_VERSION} - build configuration")
    message(STATUS "  Platform ........... ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
    message(STATUS "  Build type ......... ${CMAKE_BUILD_TYPE}")
    foreach(_line IN LISTS _lines)
        message(STATUS "${_line}")
    endforeach()
    message(STATUS "")
endfunction()

visora_summary("OpenCV" "YES" "${OPENCV_VERSION}")

if(VISORA_WITH_ROCKCHIP)
    visora_summary("Rockchip RGA" "YES" "${VISORA_RGA_LIB}")
    visora_summary("Rockchip RKNN" "YES" "${VISORA_RKNN_LIB}")
else()
    if(VISORA_RGA_LIB)
        visora_summary("Rockchip RGA" "NO " "found but RKNN missing")
    else()
        visora_summary("Rockchip RGA" "NO " "librga not found")
    endif()
    if(VISORA_RKNN_LIB)
        visora_summary("Rockchip RKNN" "NO " "found but RGA missing")
    else()
        visora_summary("Rockchip RKNN" "NO " "librknnrt not found")
    endif()
endif()
