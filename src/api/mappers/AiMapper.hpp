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

// The catalogues, built here rather than inline in the controller so their
// SHAPE can be pinned by a test. That shape is a contract with a UI which is
// deployed separately: reading `data.types` where the answer is a bare array
// gives an empty dropdown and no error anywhere, and reading `sessions` on an
// array crashes the page on an HTTP 200.
oatpp::Object<AiModelTypesDto> modelTypesDto();
oatpp::List<oatpp::Object<AiCatalogEntryDto>> transformsDto();

}  // namespace visora::api
