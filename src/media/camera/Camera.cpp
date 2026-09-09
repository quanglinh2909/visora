#include "media/camera/Camera.hpp"

#include <algorithm>
#include <cctype>

namespace visora::media {
namespace {

std::string lowered(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Assigns and reports whether anything moved.
template <class T>
bool assign(const std::optional<T>& from, T& to) {
    if (!from.has_value() || *from == to) return false;
    to = *from;
    return true;
}

}  // namespace

const char* toString(CameraState state) {
    switch (state) {
        case CameraState::Online:  return "online";
        case CameraState::Offline: return "offline";
        case CameraState::Error:   return "error";
    }
    return "offline";
}

CameraState cameraStateFromString(std::string_view text) {
    const std::string value = lowered(text);
    if (value == "online") return CameraState::Online;
    if (value == "error") return CameraState::Error;
    // Rows written before the states were collapsed still carry the old
    // detailed values; anything unrecognised is offline rather than a failure.
    if (value == "running") return CameraState::Online;
    if (value == "auth_error" || value == "unsupported_codec") return CameraState::Error;
    return CameraState::Offline;
}

const char* toString(RecordingMode mode) {
    switch (mode) {
        case RecordingMode::Off:        return "off";
        case RecordingMode::Continuous: return "continuous";
        case RecordingMode::Motion:     return "motion";
    }
    return "off";
}

RecordingMode recordingModeFromString(std::string_view text) {
    const std::string value = lowered(text);
    if (value == "continuous" || value == "always") return RecordingMode::Continuous;
    if (value == "motion") return RecordingMode::Motion;
    return RecordingMode::Off;
}

CameraDiff apply(const CameraChanges& changes, Camera& camera) {
    CameraDiff diff;

    assign(changes.name, camera.name);

    if (assign(changes.rtsp, camera.rtsp)) {
        // inputRtsp tracks rtsp unless something else resolved it; a changed
        // source has to reach the pipeline or the camera keeps streaming from
        // the old address.
        camera.inputRtsp = camera.rtsp;
        diff.sourceChanged = true;
    }
    if (assign(changes.hardware, camera.hardware)) diff.sourceChanged = true;

    if (assign(changes.recordingEnabled, camera.recordingEnabled)) diff.recordingChanged = true;
    if (assign(changes.recordingMode, camera.recordingMode)) {
        diff.recordingChanged = true;
        // The two switches say the same thing for historical reasons: the
        // boolean predates the mode. Choosing a mode is the more specific
        // statement, so it sets the boolean to match.
        //
        // Without this a camera created with recordingMode "continuous" showed
        // recordingEnabled false in the API while it was demonstrably writing
        // segments — observed on the RK3588 board. An operator cannot be shown
        // two answers to one question.
        if (!changes.recordingEnabled.has_value()) {
            camera.recordingEnabled = camera.recordingMode != RecordingMode::Off;
        }
    }
    if (assign(changes.segmentSeconds, camera.segmentSeconds)) diff.recordingChanged = true;

    if (assign(changes.motionEnabled, camera.motionEnabled)) diff.motionChanged = true;
    if (assign(changes.motionSensitivity, camera.motionSensitivity)) diff.motionChanged = true;
    if (assign(changes.motionThreshold, camera.motionThreshold)) diff.motionChanged = true;
    if (assign(changes.preMotionSeconds, camera.preMotionSeconds)) diff.motionChanged = true;
    if (assign(changes.postMotionSeconds, camera.postMotionSeconds)) diff.motionChanged = true;
    if (assign(changes.motionKeyframeOnly, camera.motionKeyframeOnly)) diff.motionChanged = true;
    if (assign(changes.motionGridX, camera.motionGridX)) diff.motionChanged = true;
    if (assign(changes.motionGridY, camera.motionGridY)) diff.motionChanged = true;
    if (assign(changes.motionCellLevels, camera.motionCellLevels)) diff.motionChanged = true;
    if (assign(changes.motionZones, camera.motionZones)) diff.motionChanged = true;

    // Neither of these touches a pipeline: they decide what happens to an event
    // after detection, so changing them must not interrupt a live stream.
    assign(changes.motionSaveEvents, camera.motionSaveEvents);
    assign(changes.retentionDays, camera.retentionDays);

    return diff;
}

}  // namespace visora::media
