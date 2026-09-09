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
# rknn_api.h is NOT vendored: Rockchip ships it marked confidential, so it does
# not belong in this repository. Obtain it from rknn-toolkit2 and either drop it
# in third_party/rknpu2/include (git-ignored) or point VISORA_RKNN_INCLUDE_DIR
# at wherever you keep it. See docs/rockchip-setup.md.
find_path(VISORA_RKNN_INCLUDE_DIR rknn_api.h
    HINTS
        "$ENV{RKNN_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/third_party/rknpu2/include"
        /usr/include/rknn
        /usr/local/include/rknn)
find_library(VISORA_RKNN_LIB rknnrt)

if(VISORA_RGA_INCLUDE_DIR AND VISORA_RGA_LIB
        AND VISORA_RKNN_INCLUDE_DIR AND VISORA_RKNN_LIB)
    set(_visora_rockchip_default ON)
else()
    set(_visora_rockchip_default OFF)
endif()

option(VISORA_WITH_ROCKCHIP "Rockchip RGA + RKNN backends" ${_visora_rockchip_default})

# --- ONNX Runtime: inference everywhere an NPU is not ------------------------
# The portable inference backend. On a workstation it runs on the CPU or, with
# the matching build, on CUDA/TensorRT/OpenVINO; on a board it is what runs a
# .onnx that the NPU toolchain has not converted.
#
# Not vendored in the repository: the official builds are large and
# per-platform. Download one from
# https://github.com/microsoft/onnxruntime/releases and unpack it into
# third_party/onnxruntime (git-ignored), or point VISORA_ONNXRUNTIME_ROOT at it.
find_path(VISORA_ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS
        "$ENV{VISORA_ONNXRUNTIME_ROOT}/include"
        "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/include"
    PATH_SUFFIXES onnxruntime)
find_library(VISORA_ONNXRUNTIME_LIB onnxruntime
    HINTS
        "$ENV{VISORA_ONNXRUNTIME_ROOT}/lib"
        "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/lib")

if(VISORA_ONNXRUNTIME_INCLUDE_DIR AND VISORA_ONNXRUNTIME_LIB)
    set(_visora_onnx_default ON)
else()
    set(_visora_onnx_default OFF)
endif()

option(VISORA_WITH_ONNXRUNTIME "ONNX Runtime inference backend" ${_visora_onnx_default})

# --- oatpp: the HTTP API and persistence layers -------------------------------
# Comes from vcpkg. Auto-detected like everything else, so a contributor working
# on pixels or pipelines builds the lower layers without it.
find_package(oatpp 1.3.0 CONFIG QUIET)
find_package(oatpp-postgresql 1.3.0 CONFIG QUIET)
find_package(oatpp-swagger 1.3.0 CONFIG QUIET)
find_package(oatpp-websocket 1.3.0 CONFIG QUIET)

if(oatpp_FOUND AND oatpp-postgresql_FOUND)
    set(_visora_api_default ON)
else()
    set(_visora_api_default OFF)
endif()

option(VISORA_WITH_API "HTTP API and PostgreSQL persistence (needs vcpkg/oatpp)"
       ${_visora_api_default})

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

if(VISORA_WITH_API)
    visora_summary("HTTP API" "YES" "oatpp ${oatpp_VERSION}")
elseif(NOT oatpp_FOUND)
    visora_summary("HTTP API" "NO " "oatpp not found - configure with the vcpkg toolchain")
else()
    visora_summary("HTTP API" "NO " "disabled by VISORA_WITH_API=OFF")
endif()

if(VISORA_WITH_ONNXRUNTIME)
    visora_summary("ONNX Runtime" "YES" "${VISORA_ONNXRUNTIME_LIB}")
elseif(NOT VISORA_ONNXRUNTIME_LIB)
    visora_summary("ONNX Runtime" "NO " "not found - see docs/onnxruntime-setup.md")
else()
    visora_summary("ONNX Runtime" "NO " "disabled by VISORA_WITH_ONNXRUNTIME=OFF")
endif()

if(VISORA_WITH_ROCKCHIP)
    visora_summary("Rockchip RGA" "YES" "${VISORA_RGA_LIB}")
    visora_summary("Rockchip RKNN" "YES" "${VISORA_RKNN_LIB}")
else()
    if(VISORA_RGA_LIB)
        visora_summary("Rockchip RGA" "NO " "found but RKNN missing")
    else()
        visora_summary("Rockchip RGA" "NO " "librga not found")
    endif()
    if(NOT VISORA_RKNN_LIB)
        visora_summary("Rockchip RKNN" "NO " "librknnrt not found")
    elseif(NOT VISORA_RKNN_INCLUDE_DIR)
        visora_summary("Rockchip RKNN" "NO " "librknnrt found, rknn_api.h missing - see docs/rockchip-setup.md")
    else()
        visora_summary("Rockchip RKNN" "NO " "found but RGA missing")
    endif()
endif()
