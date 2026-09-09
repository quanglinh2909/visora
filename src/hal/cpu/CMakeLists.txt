add_library(visora_hal_cpu OBJECT
    CpuImageOps.cpp
)
target_link_libraries(visora_hal_cpu PUBLIC visora::hal PkgConfig::OPENCV)

# $<TARGET_OBJECTS:> via target_sources, NOT target_link_libraries.
#
# CMake does not propagate an object library's object files through an
# interface (or static) library when linked — the registration object would be
# built, linked into nothing, and the backend would silently not exist. That is
# a failure with no error message anywhere, which is why hal_tests asserts the
# cpu backend is present.
target_sources(visora_backends INTERFACE $<TARGET_OBJECTS:visora_hal_cpu>)
target_link_libraries(visora_backends INTERFACE PkgConfig::OPENCV)

visora_summary("CPU image ops" "YES" "OpenCV ${OPENCV_VERSION}")
