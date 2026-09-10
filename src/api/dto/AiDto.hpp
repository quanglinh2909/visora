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
    // The same string as `id`, under the name the deployed UI reads it by.
    // Carrying both is the cheapest way to be right for a client written
    // against either, and a catalogue entry is not worth a version negotiation.
    DTO_FIELD(String, value);
    DTO_FIELD(String, label);
    DTO_FIELD(String, description);
};

// The model types, as an OBJECT with the list under `types`.
//
// Not a bare array, because that is the shape the deployed UI parses — it reads
// `data.types`, and an array gives it `undefined`, an empty dropdown and no
// error anywhere. `entries` carries the label and description beside it, which
// a bare list of strings cannot.
class AiModelTypesDto : public oatpp::DTO {
    DTO_INIT(AiModelTypesDto, DTO)

    DTO_FIELD_INFO(types) { info->description = "Valid values for a stage's modelType"; }
    DTO_FIELD(List<String>, types);
    DTO_FIELD_INFO(entries) { info->description = "The same types, with a label and a description"; }
    DTO_FIELD(List<oatpp::Object<AiCatalogEntryDto>>, entries);
};

class AiModelDto : public oatpp::DTO {
    DTO_INIT(AiModelDto, DTO)

    DTO_FIELD_INFO(path) { info->description = "Path to pass as a stage's modelPath"; }
    DTO_FIELD(String, path);
    DTO_FIELD(String, name);
    // The same string as `name`. See AiCatalogEntryDto::value.
    DTO_FIELD(String, fileName);
    DTO_FIELD_INFO(backend) { info->description = "Which backend would load it, or null"; }
    DTO_FIELD(String, backend);
    DTO_FIELD(UInt64, sizeBytes);
};

class DetectionDto : public oatpp::DTO {
    DTO_INIT(DetectionDto, DTO)

    DTO_FIELD(Float32, x1);
    DTO_FIELD(Float32, y1);
    DTO_FIELD(Float32, x2);
    DTO_FIELD(Float32, y2);
    DTO_FIELD(Float32, score);
    DTO_FIELD(Int32, classId);
    DTO_FIELD_INFO(text) { info->description = "A label, when the model carries one"; }
    DTO_FIELD(String, text);
    DTO_FIELD_INFO(stage) { info->description = "Which stage produced it"; }
    DTO_FIELD(Int32, stage);

    // These three are what a pose, segmentation or face model produces, and
    // leaving them out made the try-an-image endpoint useless for exactly the
    // model types it is most needed for: it answered a pose model with boxes
    // and no skeleton. Shaped like the socket's format so the two agree —
    // absent when empty, and the mask as hex with its grid size beside it.
    DTO_FIELD_INFO(keypoints) { info->description = "Flat (x, y, score) triples, in frame pixels"; }
    DTO_FIELD(List<Float32>, keypoints);

    DTO_FIELD_INFO(maskGrid) { info->description = "Side of the mask bitmap, when there is one"; }
    DTO_FIELD(Int32, maskGrid);
    DTO_FIELD_INFO(mask) {
        info->description = "maskGrid x maskGrid bits covering this box, row-major, low bit "
                            "of each byte first, as hex";
    }
    DTO_FIELD(String, mask);

    DTO_FIELD_INFO(embedding) { info->description = "Feature vector, for a face model"; }
    DTO_FIELD(List<Float32>, embedding);

    DTO_FIELD(List<oatpp::Object<DetectionDto>>, children);
};

class InferenceRequestDto : public oatpp::DTO {
    DTO_INIT(InferenceRequestDto, DTO)

    DTO_FIELD_INFO(image) { info->description = "A JPEG, base64-encoded"; }
    DTO_FIELD(String, image);
    DTO_FIELD(List<oatpp::Object<AiStageDto>>, stages);
};

class InferenceResultDto : public oatpp::DTO {
    DTO_INIT(InferenceResultDto, DTO)

    DTO_FIELD(List<oatpp::Object<DetectionDto>>, detections);
    DTO_FIELD_INFO(tookMs) { info->description = "Including loading the models"; }
    DTO_FIELD(Float64, tookMs);
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(DTO)
