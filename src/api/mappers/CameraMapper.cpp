#include "api/mappers/CameraMapper.hpp"

namespace visora::api {
namespace {

oatpp::String str(const std::string& value) { return oatpp::String(value.c_str()); }

// Presence is checked with getPtr(), NOT with `if (field)`.
//
// oatpp::Boolean defines operator bool() as the VALUE it holds, not as whether
// it holds one. Writing `if (field)` therefore treats an explicit `false` as
// "not supplied" — which is precisely the bug that made the predecessor turn
// recording and motion detection off whenever someone renamed a camera. The
// api tests cover both halves of this.
template <class Wrapper, class T>
void take(const Wrapper& field, std::optional<T>& target) {
    if (field.getPtr() != nullptr) target = static_cast<T>(*field);
}

void takeString(const oatpp::String& field, std::optional<std::string>& target) {
    if (field.getPtr() != nullptr) target = std::string(*field);
}

}  // namespace

oatpp::Object<CameraDto> toDto(const media::Camera& camera) {
    auto dto = CameraDto::createShared();
    dto->id = str(camera.id);
    dto->name = str(camera.name);
    dto->rtsp = str(camera.rtsp);
    dto->state = str(media::toString(camera.state));
    dto->inputRtsp = str(camera.inputRtsp);
    dto->outputRtsp = str(camera.outputRtsp);
    dto->codec = str(media::toString(camera.codec));
    dto->hardware = str(camera.hardware);
    dto->recordingEnabled = camera.recordingEnabled;
    dto->recordingMode = str(media::toString(camera.recordingMode));
    dto->motionEnabled = camera.motionEnabled;
    dto->motionSensitivity = camera.motionSensitivity;
    dto->motionThreshold = camera.motionThreshold;
    dto->preMotionSeconds = camera.preMotionSeconds;
    dto->postMotionSeconds = camera.postMotionSeconds;
    dto->segmentSeconds = camera.segmentSeconds;
    dto->streamBitrateKbps = camera.streamBitrateKbps;
    dto->motionKeyframeOnly = camera.motionKeyframeOnly;
    dto->motionGridX = camera.motionGridX;
    dto->motionGridY = camera.motionGridY;
    dto->motionCellLevels = str(camera.motionCellLevels);
    dto->motionZones = str(camera.motionZones);
    dto->motionSaveEvents = camera.motionSaveEvents;
    dto->retentionDays = camera.retentionDays;
    dto->retryCount = camera.retryCount;
    dto->lastError = str(camera.lastError);
    dto->lastChangedAt = str(camera.lastChangedAt);
    return dto;
}

oatpp::List<oatpp::Object<CameraDto>> toDtoList(const std::vector<media::Camera>& cameras) {
    auto list = oatpp::List<oatpp::Object<CameraDto>>::createShared();
    for (const media::Camera& camera : cameras) list->push_back(toDto(camera));
    return list;
}

media::CameraChanges toChanges(const oatpp::Object<CameraDto>& dto) {
    media::CameraChanges changes;
    if (!dto) return changes;

    takeString(dto->name, changes.name);
    takeString(dto->rtsp, changes.rtsp);
    takeString(dto->hardware, changes.hardware);
    take(dto->recordingEnabled, changes.recordingEnabled);
    if (dto->recordingMode.getPtr() != nullptr) {
        changes.recordingMode = media::recordingModeFromString(*dto->recordingMode);
    }
    take(dto->motionEnabled, changes.motionEnabled);
    take(dto->motionSensitivity, changes.motionSensitivity);
    take(dto->motionThreshold, changes.motionThreshold);
    take(dto->preMotionSeconds, changes.preMotionSeconds);
    take(dto->postMotionSeconds, changes.postMotionSeconds);
    take(dto->segmentSeconds, changes.segmentSeconds);
    take(dto->streamBitrateKbps, changes.streamBitrateKbps);
    take(dto->motionKeyframeOnly, changes.motionKeyframeOnly);
    take(dto->motionGridX, changes.motionGridX);
    take(dto->motionGridY, changes.motionGridY);
    takeString(dto->motionCellLevels, changes.motionCellLevels);
    takeString(dto->motionZones, changes.motionZones);
    take(dto->motionSaveEvents, changes.motionSaveEvents);
    take(dto->retentionDays, changes.retentionDays);

    // id, state, codec, outputRtsp, retryCount, lastError and lastChangedAt are
    // deliberately NOT taken. They are server-owned: a client that echoes back
    // a whole camera object must not be able to declare itself online.
    return changes;
}

}  // namespace visora::api
