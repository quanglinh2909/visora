#pragma once

// A camera's live stream, as it appears on the wire.
//
// Field names are the predecessor's, unchanged: a UI is already deployed
// against them.

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

namespace visora::api {

class StreamStatusDto : public oatpp::DTO {
    DTO_INIT(StreamStatusDto, DTO)

    DTO_FIELD_INFO(id) { info->description = "Camera id"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "Display name"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(state) { info->description = "online | offline | error"; }
    DTO_FIELD(String, state);

    DTO_FIELD_INFO(inputRtsp) { info->description = "Where the camera is read from"; }
    DTO_FIELD(String, inputRtsp);

    DTO_FIELD_INFO(outputRtsp) { info->description = "Where this server restreams it"; }
    DTO_FIELD(String, outputRtsp);

    DTO_FIELD_INFO(codec) { info->description = "h264 | h265 | unknown"; }
    DTO_FIELD(String, codec);

    DTO_FIELD_INFO(hardware) { info->description = "Codec provider requested for this camera"; }
    DTO_FIELD(String, hardware);

    DTO_FIELD(Boolean, recordingEnabled);

    DTO_FIELD_INFO(retryCount) {
        info->description = "Consecutive failed connection attempts; 0 once online";
    }
    DTO_FIELD(UInt32, retryCount);

    DTO_FIELD(String, lastError);
    DTO_FIELD(String, lastChangedAt);

    DTO_FIELD_INFO(streaming) {
        info->description =
            "Whether the server is trying to keep this camera up. Intent, not reality: "
            "'state' says what is actually happening. False means stopped by an "
            "operator (offer start); true with state 'error' means retrying.";
    }
    DTO_FIELD(Boolean, streaming);
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(DTO)
