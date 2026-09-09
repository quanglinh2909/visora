#pragma once

// AI jobs and the catalogue on the wire.

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

namespace visora::api {

class AiStageDto : public oatpp::DTO {
    DTO_INIT(AiStageDto, DTO)

    DTO_FIELD_INFO(modelType) { info->description = "A registered model type; see /ai-model-types"; }
    DTO_FIELD(String, modelType);

    DTO_FIELD_INFO(modelPath) {
        info->description =
            "Where the artefact is. The backend follows from what it is: a .rknn goes to "
            "the NPU, a .onnx to ONNX Runtime, so the same job works on both machines "
            "provided the matching file is there.";
    }
    DTO_FIELD(String, modelPath);

    DTO_FIELD_INFO(parent) {
        info->description =
            "Which earlier stage this one crops from. -1 is the whole frame, which only "
            "stage 0 may be.";
    }
    DTO_FIELD(Int32, parent);

    DTO_FIELD_INFO(inputClasses) {
        info->description = "Which of the PARENT's classes to run on. Empty means all.";
    }
    DTO_FIELD(List<Int32>, inputClasses);

    DTO_FIELD_INFO(classFilter) {
        info->description = "Which of this stage's OWN classes to keep. Empty means all.";
    }
    DTO_FIELD(List<Int32>, classFilter);

    DTO_FIELD(Float32, conf);

    DTO_FIELD_INFO(transform) { info->description = "A registered transform; empty is a plain crop"; }
    DTO_FIELD(String, transform);
};

class AiJobDto : public oatpp::DTO {
    DTO_INIT(AiJobDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, name);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(Boolean, enabled);

    DTO_FIELD_INFO(maxFps) { info->description = "0 means as fast as frames arrive"; }
    DTO_FIELD(Int32, maxFps);

    DTO_FIELD(List<oatpp::Object<AiStageDto>>, stages);

    // --- runtime, absent when the job is not loaded ---
    DTO_FIELD_INFO(running) { info->description = "Whether the job is analysing right now"; }
    DTO_FIELD(Boolean, running);
    DTO_FIELD_INFO(lastError) { info->description = "Why it is not running, when it is not"; }
    DTO_FIELD(String, lastError);
    DTO_FIELD(UInt64, framesAnalysed);
    DTO_FIELD(UInt64, resultsPublished);
    DTO_FIELD_INFO(lastInferenceMs) {
        info->description = "How long the last inference took — whether the accelerator keeps up";
    }
    DTO_FIELD(Float64, lastInferenceMs);
};

class AiCatalogEntryDto : public oatpp::DTO {
    DTO_INIT(AiCatalogEntryDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, label);
    DTO_FIELD(String, description);
};

class AiModelDto : public oatpp::DTO {
    DTO_INIT(AiModelDto, DTO)

    DTO_FIELD_INFO(path) { info->description = "Path to pass as a stage's modelPath"; }
    DTO_FIELD(String, path);
    DTO_FIELD(String, name);
    DTO_FIELD_INFO(backend) { info->description = "Which backend would load it, or null"; }
    DTO_FIELD(String, backend);
    DTO_FIELD(UInt64, sizeBytes);
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(DTO)
