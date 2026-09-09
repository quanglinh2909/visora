#include "api/mappers/RecordingMapper.hpp"

#include "core/Time.hpp"

namespace visora::api {
namespace {

// The same shape the playlist uses, so a client can match a segment it sees in
// one against the other.
std::string segmentUrl(const std::string& id) { return "/recording-segments/" + id + "/file"; }

}  // namespace

oatpp::Object<RecordingSegmentDto> toDto(const media::RecordingSegment& segment) {
    auto dto = RecordingSegmentDto::createShared();
    dto->id = segment.id;
    dto->cameraId = segment.cameraId;
    dto->startAt = core::toIso8601Millis(segment.startMs);
    dto->endAt = core::toIso8601Millis(segment.endMs);
    dto->durationMs = segment.durationMs;
    dto->codec = media::toString(segment.codec);
    dto->container = segment.container;
    dto->recordingMode = media::toString(segment.recordingMode);
    dto->hasMotion = segment.hasMotion;
    // Absent rather than empty: a client checking for a linked event should not
    // have to distinguish "" from null.
    if (!segment.motionEventId.empty()) dto->motionEventId = segment.motionEventId;
    dto->status = media::toString(segment.status);
    dto->url = segmentUrl(segment.id);
    // The path on disk is deliberately NOT sent. It is of no use to a client
    // and tells anyone who sees it how the server's filesystem is laid out.
    return dto;
}

oatpp::List<oatpp::Object<RecordingSegmentDto>> toDtoList(
    const std::vector<media::RecordingSegment>& segments) {
    auto list = oatpp::List<oatpp::Object<RecordingSegmentDto>>::createShared();
    for (const auto& segment : segments) list->push_back(toDto(segment));
    return list;
}

oatpp::Object<MotionEventDto> toDto(const media::MotionEvent& event) {
    auto dto = MotionEventDto::createShared();
    dto->id = event.id;
    dto->cameraId = event.cameraId;
    dto->startAt = core::toIso8601Millis(event.startMs);
    // An open event has no end. Absent says that; a zero epoch would say 1970.
    if (event.endMs != 0) dto->endAt = core::toIso8601Millis(event.endMs);
    dto->maxScore = event.maxScore;
    dto->cells = event.cells;
    dto->gridX = event.gridX;
    dto->gridY = event.gridY;
    if (!event.imagePath.empty()) {
        dto->imageUrl = "/motion-events/" + event.id + "/image";
    }
    return dto;
}

oatpp::List<oatpp::Object<MotionEventDto>> toDtoList(
    const std::vector<media::MotionEvent>& events) {
    auto list = oatpp::List<oatpp::Object<MotionEventDto>>::createShared();
    for (const auto& event : events) list->push_back(toDto(event));
    return list;
}

oatpp::Object<SeekResultDto> toDto(const media::SeekPoint& point) {
    auto dto = SeekResultDto::createShared();
    dto->segment = toDto(point.segment);
    dto->offsetMs = point.offsetMs;
    dto->startAt = core::toIso8601Millis(point.segment.startMs + point.offsetMs);
    return dto;
}

}  // namespace visora::api
