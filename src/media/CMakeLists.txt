# libmedia — GStreamer pipelines, camera sessions, recording, playback.
#
# Pipeline DESCRIPTION (ElementSpec, LaunchPipeline, Codec) needs no GStreamer
# and is pure enough to test anywhere. Only the availability probe links it, so
# most of this layer is exercised on a machine with no plugins installed.
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-rtsp-1.0
    gstreamer-rtsp-server-1.0
    gstreamer-app-1.0
    gstreamer-sdp-1.0
    gstreamer-rtp-1.0
    gstreamer-webrtc-1.0
    gstreamer-video-1.0
)

add_library(visora_media STATIC
    gst/BusWatcher.cpp
    gst/Codec.cpp
    gst/CodecProvider.cpp
    gst/ElementAvailability.cpp
    gst/ElementSpec.cpp
    gst/LaunchPipeline.cpp
    pipeline/CameraPipelines.cpp
    camera/Camera.cpp
    recording/Recording.cpp
    recording/HlsPlaylist.cpp
    recording/MotionGate.cpp
    recording/PlaybackService.cpp
    recording/RecordingManager.cpp
    recording/RecordingSession.cpp
    recording/ThumbnailExtractor.cpp
    ai/AiRuntime.cpp
    ai/FrameTap.cpp
    ai/JpegDecoder.cpp
    ai/JpegEncoder.cpp
    ai/MotionEventRecorder.cpp
    ai/MotionSnapshotWriter.cpp
    moq/MoqFeed.cpp
    moq/MoqService.cpp
    webrtc/Sdp.cpp
    webrtc/WebRtcService.cpp
    webrtc/WhepSession.cpp
    webrtc/WhepPipeline.cpp
    source/CameraSourceRegistry.cpp
    source/PlaybackSource.cpp
    source/RtspEncodedSource.cpp
    source/SinkFanout.cpp
    source/TranscodedSource.cpp
    camera/CameraRuntime.cpp
    camera/CameraService.cpp
    stream/CodecProbe.cpp
    stream/RtspServer.cpp
    stream/SnapshotGrabber.cpp
    stream/StreamManager.cpp
    stream/RetryPolicy.cpp
)
add_library(visora::media ALIAS visora_media)

target_link_libraries(visora_media PUBLIC visora::core visora::vision PkgConfig::GSTREAMER)

# Codec providers are plug-ins, collected like every other backend: an OBJECT
# library reaching consumers through target_sources. See docs/ARCHITECTURE.md.
add_library(visora_media_providers OBJECT
    gst/providers/NvidiaCodecProvider.cpp
    gst/providers/RockchipCodecProvider.cpp
    gst/providers/SoftwareCodecProvider.cpp
    gst/providers/V4l2CodecProvider.cpp
    gst/providers/VaapiCodecProvider.cpp
)
target_link_libraries(visora_media_providers PUBLIC visora::media)
target_sources(visora_backends INTERFACE $<TARGET_OBJECTS:visora_media_providers>)
target_link_libraries(visora_backends INTERFACE visora::media)

visora_summary("GStreamer" "YES" "${GSTREAMER_VERSION}")
