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

}  // namespace visora::api

#include OATPP_CODEGEN_END(DTO)
