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

    DTO_FIELD_INFO(clientAddr) { info->description = "The browser's address, as the server saw it"; }
    DTO_FIELD(String, clientAddr);

    DTO_FIELD_INFO(mode) { info->description = "'live' for a camera, 'playback' for a recording"; }
    DTO_FIELD(String, mode);

    DTO_FIELD_INFO(connected) {
        info->description = "Whether the peer connection actually came up. An offer that "
                            "never connected is a session too, and looks identical without this.";
    }
    DTO_FIELD(Boolean, connected);

    DTO_FIELD_INFO(ageMs) { info->description = "How long this session has been open"; }
    DTO_FIELD(Int64, ageMs);
    DTO_FIELD(Int64, ageSeconds);
};

// The viewers response is an OBJECT, not a bare array.
//
// It carries the counts beside the list because that is what a dashboard shows
// — and because the shape is the predecessor's, which a deployed frontend
// already parses. Returning the array alone gave that frontend an HTTP 200 it
// could not read, and a page that crashes on a 200 is worse than one that
// handles a 404.
class ViewersDto : public oatpp::DTO {
    DTO_INIT(ViewersDto, DTO)

    DTO_FIELD(Int64, total);
    DTO_FIELD_INFO(live) { info->description = "Sessions watching a camera"; }
    DTO_FIELD(Int64, live);
    DTO_FIELD_INFO(playback) { info->description = "Sessions watching a recording"; }
    DTO_FIELD(Int64, playback);
    DTO_FIELD(List<oatpp::Object<ViewerDto>>, sessions);
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
