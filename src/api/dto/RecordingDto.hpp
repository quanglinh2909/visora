#pragma once

// Recordings and motion events on the wire.
//
// Instants are ISO-8601 strings here and milliseconds internally: a client
// wants something it can display and a timeline wants something it can
// subtract, and converting at the edge is cheaper than being wrong in the
// middle.

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

namespace visora::api {

class RecordingSegmentDto : public oatpp::DTO {
    DTO_INIT(RecordingSegmentDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(String, startAt);
    DTO_FIELD(String, endAt);
    DTO_FIELD(Int32, durationMs);
    DTO_FIELD(String, codec);
    DTO_FIELD(String, container);
    DTO_FIELD(String, recordingMode);
    DTO_FIELD(Boolean, hasMotion);
    DTO_FIELD(String, motionEventId);

    DTO_FIELD_INFO(status) {
        info->description = "recording = still being written; complete = finished";
    }
    DTO_FIELD(String, status);

    DTO_FIELD_INFO(url) { info->description = "Where to fetch this segment's bytes"; }
    DTO_FIELD(String, url);
};

class SeekResultDto : public oatpp::DTO {
    DTO_INIT(SeekResultDto, DTO)

    DTO_FIELD(oatpp::Object<RecordingSegmentDto>, segment);

    DTO_FIELD_INFO(offsetMs) {
        info->description =
            "How far into the segment to start. Zero when the requested instant fell "
            "in a gap and the next recording is used instead.";
    }
    DTO_FIELD(Int64, offsetMs);

    DTO_FIELD_INFO(startAt) { info->description = "The instant playback will actually start"; }
    DTO_FIELD(String, startAt);
};

class MotionEventDto : public oatpp::DTO {
    DTO_INIT(MotionEventDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(String, startAt);
    DTO_FIELD_INFO(endAt) { info->description = "Absent while the event is still happening"; }
    DTO_FIELD(String, endAt);
    DTO_FIELD(Float64, maxScore);
    DTO_FIELD_INFO(cells) { info->description = "Cells that moved, \"row:col\" comma-separated"; }
    DTO_FIELD(String, cells);
    DTO_FIELD(Int32, gridX);
    DTO_FIELD(Int32, gridY);
    DTO_FIELD_INFO(imageUrl) { info->description = "Absent when the event has no picture"; }
    DTO_FIELD(String, imageUrl);
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(DTO)
