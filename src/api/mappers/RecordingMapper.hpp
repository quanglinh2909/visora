#pragma once

// Domain -> DTO for recordings and motion events. One direction: nothing on the
// wire creates a segment, they are produced by the recorder.

#include <vector>

#include "api/dto/RecordingDto.hpp"
#include "media/recording/Recording.hpp"

namespace visora::api {

oatpp::Object<RecordingSegmentDto> toDto(const media::RecordingSegment& segment);
oatpp::List<oatpp::Object<RecordingSegmentDto>> toDtoList(
    const std::vector<media::RecordingSegment>& segments);

oatpp::Object<MotionEventDto> toDto(const media::MotionEvent& event);
oatpp::List<oatpp::Object<MotionEventDto>> toDtoList(
    const std::vector<media::MotionEvent>& events);

oatpp::Object<SeekResultDto> toDto(const media::SeekPoint& point);

}  // namespace visora::api
