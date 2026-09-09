#pragma once

// Domain <-> DTO for AI jobs.

#include <vector>

#include "api/dto/AiDto.hpp"
#include "media/ai/AiRuntime.hpp"
#include "vision/AiJob.hpp"
#include "vision/Detection.hpp"

namespace visora::api {

// The runtime status is optional: a job that has never been loaded has none,
// and reporting zeros would look like a job that is running and finding
// nothing.
oatpp::Object<AiJobDto> toDto(const vision::AiJob& job,
                              const media::AiJobStatus* status = nullptr);
oatpp::List<oatpp::Object<AiJobDto>> toDtoList(
    const std::vector<vision::AiJob>& jobs,
    const std::vector<media::AiJobStatus>& statuses);

vision::AiJobChanges toChanges(const oatpp::Object<AiJobDto>& dto);

oatpp::Object<DetectionDto> toDetectionDto(const vision::Detection& detection);
oatpp::List<oatpp::Object<DetectionDto>> toDetectionDtoList(
    const std::vector<vision::Detection>& detections);

}  // namespace visora::api
