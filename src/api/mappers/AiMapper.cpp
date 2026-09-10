#include "api/mappers/AiMapper.hpp"

#include "vision/ModelType.hpp"
#include "vision/Transform.hpp"

#include <string>

namespace visora::api {
namespace {

std::vector<int> toInts(const oatpp::List<oatpp::Int32>& list) {
    std::vector<int> out;
    if (!list) return out;
    for (const auto& value : *list) {
        if (value) out.push_back(*value);
    }
    return out;
}

oatpp::List<oatpp::Int32> fromInts(const std::vector<int>& values) {
    auto list = oatpp::List<oatpp::Int32>::createShared();
    for (const int value : values) list->push_back(value);
    return list;
}

}  // namespace

oatpp::Object<AiJobDto> toDto(const vision::AiJob& job, const media::AiJobStatus* status) {
    auto dto = AiJobDto::createShared();
    dto->id = job.id;
    dto->name = job.name;
    dto->cameraId = job.cameraId;
    dto->enabled = job.enabled;
    dto->maxFps = job.maxFps;

    auto stages = oatpp::List<oatpp::Object<AiStageDto>>::createShared();
    for (const vision::AiStage& stage : job.stages) {
        auto stageDto = AiStageDto::createShared();
        stageDto->modelType = stage.modelType;
        stageDto->modelPath = stage.modelPath;
        stageDto->parent = stage.parent;
        stageDto->inputClasses = fromInts(stage.inputClasses);
        stageDto->classFilter = fromInts(stage.classFilter);
        stageDto->conf = stage.confidence;
        stageDto->transform = stage.transform;
        stages->push_back(stageDto);
    }
    dto->stages = stages;

    if (status) {
        dto->running = status->running;
        // Absent rather than empty, so a client checking for a problem does not
        // have to distinguish "" from null.
        if (!status->lastError.empty()) dto->lastError = status->lastError;
        dto->framesAnalysed = status->framesAnalysed;
        dto->resultsPublished = status->resultsPublished;
        dto->lastInferenceMs = status->lastInferenceMs;
    }
    return dto;
}

oatpp::List<oatpp::Object<AiJobDto>> toDtoList(
    const std::vector<vision::AiJob>& jobs,
    const std::vector<media::AiJobStatus>& statuses) {
    auto list = oatpp::List<oatpp::Object<AiJobDto>>::createShared();
    for (const vision::AiJob& job : jobs) {
        const media::AiJobStatus* found = nullptr;
        for (const media::AiJobStatus& status : statuses) {
            if (status.jobId == job.id) {
                found = &status;
                break;
            }
        }
        list->push_back(toDto(job, found));
    }
    return list;
}

oatpp::Object<DetectionDto> toDetectionDto(const vision::Detection& detection) {
    auto dto = DetectionDto::createShared();
    dto->x1 = detection.x1;
    dto->y1 = detection.y1;
    dto->x2 = detection.x2;
    dto->y2 = detection.y2;
    dto->score = detection.score;
    dto->classId = detection.classId;
    if (!detection.text.empty()) dto->text = detection.text;
    dto->stage = detection.stage;

    // Absent when empty, so a plain detection job pays nothing for features it
    // does not use — the same frugality the socket format has.
    if (!detection.keypoints.empty()) {
        auto keypoints = oatpp::List<oatpp::Float32>::createShared();
        for (const float value : detection.keypoints) keypoints->push_back(value);
        dto->keypoints = keypoints;
    }
    if (!detection.maskBits.empty()) {
        static const char* kHex = "0123456789abcdef";
        std::string hex;
        hex.reserve(detection.maskBits.size() * 2);
        for (const unsigned char byte : detection.maskBits) {
            hex.push_back(kHex[byte >> 4]);
            hex.push_back(kHex[byte & 0x0F]);
        }
        dto->maskGrid = vision::Detection::kMaskGrid;
        dto->mask = hex.c_str();
    }
    if (!detection.embedding.empty()) {
        auto embedding = oatpp::List<oatpp::Float32>::createShared();
        for (const float value : detection.embedding) embedding->push_back(value);
        dto->embedding = embedding;
    }
    dto->children = toDetectionDtoList(detection.children);
    return dto;
}

oatpp::List<oatpp::Object<DetectionDto>> toDetectionDtoList(
    const std::vector<vision::Detection>& detections) {
    auto list = oatpp::List<oatpp::Object<DetectionDto>>::createShared();
    for (const vision::Detection& detection : detections) {
        list->push_back(toDetectionDto(detection));
    }
    return list;
}

vision::AiJobChanges toChanges(const oatpp::Object<AiJobDto>& dto) {
    vision::AiJobChanges changes;
    if (!dto) return changes;

    // Presence is `getPtr() != nullptr`, never `if (field)`: oatpp::Boolean's
    // operator bool returns the VALUE, so `if (enabled)` treats an explicit
    // false as "not supplied" — which is exactly the partial-update bug the
    // camera mapper documents.
    if (dto->name.getPtr() != nullptr) changes.name = dto->name->c_str();
    if (dto->cameraId.getPtr() != nullptr) changes.cameraId = dto->cameraId->c_str();
    if (dto->enabled.getPtr() != nullptr) changes.enabled = *dto->enabled;
    if (dto->maxFps.getPtr() != nullptr) changes.maxFps = *dto->maxFps;

    if (dto->stages.getPtr() != nullptr) {
        std::vector<vision::AiStage> stages;
        for (const auto& stageDto : *dto->stages) {
            if (!stageDto) continue;
            vision::AiStage stage;
            if (stageDto->modelType.getPtr() != nullptr) {
                stage.modelType = stageDto->modelType->c_str();
            }
            if (stageDto->modelPath.getPtr() != nullptr) {
                stage.modelPath = stageDto->modelPath->c_str();
            }
            // Default -1, so a single-stage job needs no parent field at all.
            stage.parent = stageDto->parent.getPtr() != nullptr ? *stageDto->parent : -1;
            stage.inputClasses = toInts(stageDto->inputClasses);
            stage.classFilter = toInts(stageDto->classFilter);
            if (stageDto->conf.getPtr() != nullptr) stage.confidence = *stageDto->conf;
            if (stageDto->transform.getPtr() != nullptr) {
                stage.transform = stageDto->transform->c_str();
            }
            stages.push_back(std::move(stage));
        }
        changes.stages = std::move(stages);
    }
    // id, and the runtime fields, are server-owned and deliberately not taken.
    return changes;
}

oatpp::Object<AiModelTypesDto> modelTypesDto() {
    auto entries = oatpp::List<oatpp::Object<AiCatalogEntryDto>>::createShared();
    auto types = oatpp::List<oatpp::String>::createShared();
    for (const vision::ModelType* type : vision::modelTypes()) {
        auto dto = AiCatalogEntryDto::createShared();
        dto->id = std::string(type->id());
        dto->value = dto->id;
        dto->label = std::string(type->label());
        dto->description = std::string(type->description());
        entries->push_back(dto);
        types->push_back(dto->id);
    }
    auto out = AiModelTypesDto::createShared();
    out->types = types;
    out->entries = entries;
    return out;
}

oatpp::List<oatpp::Object<AiCatalogEntryDto>> transformsDto() {
    auto list = oatpp::List<oatpp::Object<AiCatalogEntryDto>>::createShared();
    for (const vision::Transform* item : vision::transforms()) {
        auto dto = AiCatalogEntryDto::createShared();
        dto->id = std::string(item->id());
        dto->value = dto->id;
        dto->label = std::string(item->label());
        dto->description = std::string(item->description());
        list->push_back(dto);
    }
    return list;
}

}  // namespace visora::api
