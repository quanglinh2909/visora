#pragma once

// MoQ feeds on the wire.

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

namespace visora::api {

class MoqFeedRequestDto : public oatpp::DTO {
    DTO_INIT(MoqFeedRequestDto, DTO)

    DTO_FIELD_INFO(feed) { info->description = "The MoQ server's own id for this feed"; }
    DTO_FIELD(String, feed);

    DTO_FIELD(String, cameraId);

    DTO_FIELD_INFO(mode) { info->description = "live | playback"; }
    DTO_FIELD(String, mode);

    DTO_FIELD_INFO(at) { info->description = "Where playback starts, ISO-8601. Playback only."; }
    DTO_FIELD(String, at);
};

class MoqFeedDto : public oatpp::DTO {
    DTO_INIT(MoqFeedDto, DTO)

    DTO_FIELD(String, sessionId);
    DTO_FIELD(String, feed);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(String, mode);

    DTO_FIELD(UInt64, framesSent);

    DTO_FIELD_INFO(framesDropped) {
        info->description =
            "Frames dropped because the MoQ server was not reading fast enough. A slow "
            "reader loses picture; it never slows down the camera or any other viewer.";
    }
    DTO_FIELD(UInt64, framesDropped);
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(DTO)
