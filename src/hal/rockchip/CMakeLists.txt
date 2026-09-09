add_library(visora_hal_rockchip OBJECT
    DmaHeapBuffer.cpp
    RgaImageOps.cpp
    RknnBackend.cpp
)
target_link_libraries(visora_hal_rockchip PUBLIC visora::hal)
target_include_directories(visora_hal_rockchip PUBLIC
    "${VISORA_RGA_INCLUDE_DIR}"
    "${VISORA_RKNN_INCLUDE_DIR}"
)

# OBJECT library wired through target_sources, never target_link_libraries —
# see docs/ARCHITECTURE.md. Getting this wrong drops the registration silently.
target_sources(visora_backends INTERFACE $<TARGET_OBJECTS:visora_hal_rockchip>)
target_link_libraries(visora_backends INTERFACE "${VISORA_RGA_LIB}" "${VISORA_RKNN_LIB}")

visora_summary("RGA image ops" "YES" "hardware 2D")
visora_summary("RKNN inference" "YES" "NPU")
