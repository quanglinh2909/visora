#pragma once

// WebRTC viewers on the wire.

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

namespace visora::api {

class ViewerDto : public oatpp::DTO {
    DTO_INIT(ViewerDto, DTO)

    DTO_FIELD(String, sessionId);
    DTO_FIELD(String, cameraId);

    DTO_FIELD_INFO(codec) { info->description = "What is being SENT, which differs when transcoding"; }
    DTO_FIELD(String, codec);

    DTO_FIELD_INFO(transcoded) {
        info->description =
            "True when the camera's codec had to be re-encoded for this browser. "
            "Worth watching: transcoding costs far more than passthrough.";
    }
    DTO_FIELD(Boolean, transcoded);

    DTO_FIELD_INFO(rtpPackets) { info->description = "Packets sent so far; 0 means nothing flowed"; }
    DTO_FIELD(UInt64, rtpPackets);

    DTO_FIELD(String, startedAt);
};

class PlaybackControlDto : public oatpp::DTO {
    DTO_INIT(PlaybackControlDto, DTO)

    DTO_FIELD_INFO(action) { info->description = "seek | pause | resume | rate"; }
    DTO_FIELD(String, action);

    DTO_FIELD_INFO(at) { info->description = "Where to seek to, ISO-8601. Only for 'seek'."; }
    DTO_FIELD(String, at);

    DTO_FIELD_INFO(rate) {
        info->description =
            "Playback speed. Only for 'rate'. At 4x and above only keyframes are sent, "
            "so scrubbing gets smoother as it gets faster rather than choppier.";
    }
    DTO_FIELD(Float64, rate);
};

class PlaybackStatusDto : public oatpp::DTO {
    DTO_INIT(PlaybackStatusDto, DTO)

    DTO_FIELD(String, sessionId);
    DTO_FIELD_INFO(position) { info->description = "Where playback has reached, ISO-8601"; }
    DTO_FIELD(String, position);
    DTO_FIELD(Float64, rate);
    DTO_FIELD(Boolean, paused);
    DTO_FIELD_INFO(ended) {
        info->description =
            "True when there is nothing more recorded from here. The session stays open: "
            "seeking elsewhere resumes it.";
    }
    DTO_FIELD(Boolean, ended);
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(DTO)
