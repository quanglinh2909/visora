# ONNX Runtime backend — an OBJECT library, like every other backend.
#
# See docs/ARCHITECTURE.md for why this must not be a static archive: nothing
# references it by symbol, so the linker would drop it and the backend would
# vanish with no error.
add_library(visora_hal_onnxruntime OBJECT
    OnnxRuntimeBackend.cpp
)
target_link_libraries(visora_hal_onnxruntime PUBLIC visora::hal)
target_include_directories(visora_hal_onnxruntime PRIVATE "${VISORA_ONNXRUNTIME_INCLUDE_DIR}")

target_sources(visora_backends INTERFACE $<TARGET_OBJECTS:visora_hal_onnxruntime>)
target_link_libraries(visora_backends INTERFACE "${VISORA_ONNXRUNTIME_LIB}")
