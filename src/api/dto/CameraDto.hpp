#pragma once

// The camera resource as it appears on the wire.
//
// Field names and types are the predecessor's, unchanged: a UI is already
// deployed against them. This file is the only place in the project that
// describes that shape — the domain entity is free to differ, and the mapper
// reconciles them.

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

namespace visora::api {

class CameraDto : public oatpp::DTO {
    DTO_INIT(CameraDto, DTO)

    DTO_FIELD_INFO(id) { info->description = "Camera id (UUID, server-generated)"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "Display name"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(rtsp) { info->description = "RTSP stream URL"; }
    DTO_FIELD(String, rtsp);

    DTO_FIELD_INFO(state) { info->description = "online | offline | error"; }
    DTO_FIELD(String, state);

    DTO_FIELD_INFO(inputRtsp) { info->description = "Input RTSP URL used by the runtime stream"; }
    DTO_FIELD(String, inputRtsp);

    DTO_FIELD_INFO(outputRtsp) { info->description = "Output RTSP URL exposed by this service"; }
    DTO_FIELD(String, outputRtsp);

    DTO_FIELD_INFO(codec) { info->description = "Detected stream codec"; }
    DTO_FIELD(String, codec);

    DTO_FIELD_INFO(hardware) {
        info->description = "Acceleration preference: auto | software | vaapi | nvdec | v4l2 | mpp";
    }
    DTO_FIELD(String, hardware);

    DTO_FIELD_INFO(recordingEnabled) { info->description = "Whether recording is enabled"; }
    DTO_FIELD(Boolean, recordingEnabled);

    DTO_FIELD_INFO(recordingMode) { info->description = "off | continuous | motion"; }
    DTO_FIELD(String, recordingMode);

    DTO_FIELD_INFO(motionEnabled) { info->description = "Whether motion detection is enabled"; }
    DTO_FIELD(Boolean, motionEnabled);

    DTO_FIELD_INFO(motionSensitivity) { info->description = "motioncells sensitivity, 0..1"; }
    DTO_FIELD(Float64, motionSensitivity);

    DTO_FIELD_INFO(motionThreshold) { info->description = "motioncells threshold, 0..1"; }
    DTO_FIELD(Float64, motionThreshold);

    DTO_FIELD_INFO(preMotionSeconds) { info->description = "Seconds retained before motion starts"; }
    DTO_FIELD(Int32, preMotionSeconds);

    DTO_FIELD_INFO(postMotionSeconds) { info->description = "Seconds retained after motion ends"; }
    DTO_FIELD(Int32, postMotionSeconds);

    DTO_FIELD_INFO(segmentSeconds) { info->description = "Recording segment duration in seconds"; }
    DTO_FIELD(Int32, segmentSeconds);

    DTO_FIELD_INFO(motionKeyframeOnly) {
        info->description = "Analyse only keyframes in the motion branch";
    }
    DTO_FIELD(Boolean, motionKeyframeOnly);

    DTO_FIELD_INFO(motionGridX) { info->description = "Motion grid columns (8..32)"; }
    DTO_FIELD(Int32, motionGridX);

    DTO_FIELD_INFO(motionGridY) { info->description = "Motion grid rows (8..32)"; }
    DTO_FIELD(Int32, motionGridY);

    DTO_FIELD_INFO(motionCellLevels) {
        info->description = "One digit per cell, row-major: 0 = ignore, 1..9 = level";
    }
    DTO_FIELD(String, motionCellLevels);

    DTO_FIELD_INFO(motionZones) {
        info->description =
            "Motion zones as JSON [{r1,c1,r2,c2,level}]. Grid-cell coordinates, "
            "inclusive at both ends. level 1..10 requires level*10% of that "
            "zone's own cells to move.";
    }
    DTO_FIELD(String, motionZones);

    DTO_FIELD_INFO(motionSaveEvents) {
        info->description = "Write motion events to the database, or only push them over the websocket";
    }
    DTO_FIELD(Boolean, motionSaveEvents);

    DTO_FIELD_INFO(retentionDays) { info->description = "Days of recordings to keep; 0 = keep all"; }
    DTO_FIELD(Int32, retentionDays);

    DTO_FIELD_INFO(retryCount) { info->description = "Consecutive reconnect attempts"; }
    DTO_FIELD(Int32, retryCount);

    DTO_FIELD_INFO(lastError) { info->description = "Why the stream last failed, if it did"; }
    DTO_FIELD(String, lastError);

    DTO_FIELD_INFO(lastChangedAt) { info->description = "When the runtime state last changed (UTC)"; }
    DTO_FIELD(String, lastChangedAt);
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(DTO)
