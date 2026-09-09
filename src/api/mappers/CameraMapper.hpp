#pragma once

// Domain <-> DTO for cameras.
//
// The only code that knows both shapes. Keeping it here means the entity can
// gain a field without the wire changing, and the wire can be reshaped without
// the business rules noticing — which is the point of having two types at all
// rather than serialising the entity directly.

#include "api/dto/CameraDto.hpp"
#include "media/camera/Camera.hpp"

namespace visora::api {

oatpp::Object<CameraDto> toDto(const media::Camera& camera);
oatpp::List<oatpp::Object<CameraDto>> toDtoList(const std::vector<media::Camera>& cameras);

// A DTO carries every field as nullable, which is exactly what a partial update
// needs: a field the client omitted stays absent in CameraChanges and the
// service leaves it alone. The predecessor could not express that and reset
// omitted booleans to false.
media::CameraChanges toChanges(const oatpp::Object<CameraDto>& dto);

}  // namespace visora::api
