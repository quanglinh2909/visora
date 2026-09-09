# libhal — the extension mechanism and the interfaces. No vendor headers here.
add_library(visora_hal STATIC
    Capabilities.cpp
    ImageOps.cpp
    InferenceBackend.cpp
)
add_library(visora::hal ALIAS visora_hal)

target_link_libraries(visora_hal PUBLIC visora::core)

# Backend modules are OBJECT libraries collected here.
#
# They must NOT go into a static archive: their only job is to run a static
# registration object, nothing references them by symbol, and the linker would
# drop the archive member — the backend would vanish with no error at all.
# Consumers link visora::backends, which carries the object files directly.
add_library(visora_backends INTERFACE)
add_library(visora::backends ALIAS visora_backends)

add_subdirectory(cpu)

if(VISORA_WITH_ROCKCHIP)
    add_subdirectory(rockchip)
endif()

if(VISORA_WITH_ONNXRUNTIME)
    add_subdirectory(onnxruntime)
endif()
