#include "api/mappers/AiMapper.hpp"

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

}  // namespace visora::api
