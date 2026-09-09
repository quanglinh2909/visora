# libvision — inference results, jobs and the AI pipeline.
#
# Talks to hardware only through hal interfaces, so it builds and its logic
# tests run on a machine with no accelerator.
add_library(visora_vision STATIC
    ResultSink.cpp
    ResultWire.cpp
)
add_library(visora::vision ALIAS visora_vision)

target_link_libraries(visora_vision PUBLIC visora::core)

# Result sinks are plug-ins, registered the same way hardware backends are, and
# collected the same way — an OBJECT library reaching consumers through
# target_sources. See docs/ARCHITECTURE.md.
add_library(visora_vision_sinks OBJECT
    sinks/UnixSocketSink.cpp
)
target_link_libraries(visora_vision_sinks PUBLIC visora::vision)
target_sources(visora_backends INTERFACE $<TARGET_OBJECTS:visora_vision_sinks>)
target_link_libraries(visora_backends INTERFACE visora::vision)
