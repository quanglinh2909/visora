#pragma once

// Domain -> DTO for live stream status. One direction only: nothing on the wire
// sets a runtime state, it is observed.

#include <vector>

#include "api/dto/StreamStatusDto.hpp"
#include "media/camera/CameraRuntime.hpp"

namespace visora::api {

oatpp::Object<StreamStatusDto> toDto(const media::CameraRuntimeStatus& status);
oatpp::List<oatpp::Object<StreamStatusDto>> toDtoList(
    const std::vector<media::CameraRuntimeStatus>& statuses);

}  // namespace visora::api
