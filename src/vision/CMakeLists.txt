# libvision — inference results, jobs and the AI pipeline.
#
# Talks to hardware only through hal interfaces, so it builds and its logic
# tests run on a machine with no accelerator.
add_library(visora_vision STATIC
    AiJob.cpp
    AiJobService.cpp
    ModelType.cpp
    MotionDetector.cpp
    StageRunner.cpp
    ResultSink.cpp
    ResultWire.cpp
    Transform.cpp
)
add_library(visora::vision ALIAS visora_vision)

target_link_libraries(visora_vision PUBLIC visora::core visora::hal)

# Result sinks are plug-ins, registered the same way hardware backends are, and
# collected the same way — an OBJECT library reaching consumers through
# target_sources. See docs/ARCHITECTURE.md.
add_library(visora_vision_sinks OBJECT
    sinks/UnixSocketSink.cpp
    models/FaceEmbedding.cpp
    models/PpOcrDetect.cpp
    models/PpOcrRecognise.cpp
    models/Yolov8Detect.cpp
    models/Yolov8Pose.cpp
    models/Yolov8Seg.cpp
    transforms/CropTransform.cpp
    transforms/FaceAlignTransform.cpp
    transforms/PlateAlignTransform.cpp
)
target_link_libraries(visora_vision_sinks PUBLIC visora::vision)
target_sources(visora_backends INTERFACE $<TARGET_OBJECTS:visora_vision_sinks>)
target_link_libraries(visora_backends INTERFACE visora::vision)
