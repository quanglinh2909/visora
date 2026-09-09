#pragma once

// AI jobs and the catalogue.
//
// The catalogue endpoints read the registries directly, so a model type or a
// transform added as a new file appears here without this file changing. That
// is the whole point of registering rather than listing: the predecessor had to
// edit the factory, a parallel name vector AND the validation that read them.

#include <memory>
#include <string>
#include <utility>

#include "api/HttpError.hpp"
#include "api/mappers/AiMapper.hpp"
#include "hal/InferenceBackend.hpp"
#include "media/ai/AiRuntime.hpp"
#include "vision/AiJobService.hpp"
#include "vision/ModelType.hpp"
#include "vision/Transform.hpp"

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include <filesystem>

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace visora::api {

class AiController : public oatpp::web::server::api::ApiController {
public:
    AiController(std::shared_ptr<ObjectMapper> objectMapper,
                 std::shared_ptr<vision::AiJobService> jobs,
                 std::shared_ptr<media::AiRuntime> runtime, std::string modelDir)
        : oatpp::web::server::api::ApiController(std::move(objectMapper)),
          m_jobs(std::move(jobs)),
          m_runtime(std::move(runtime)),
          m_modelDir(std::move(modelDir)) {}

    static std::shared_ptr<AiController> createShared(
        std::shared_ptr<ObjectMapper> objectMapper,
        std::shared_ptr<vision::AiJobService> jobs, std::shared_ptr<media::AiRuntime> runtime,
        std::string modelDir) {
        return std::make_shared<AiController>(std::move(objectMapper), std::move(jobs),
                                              std::move(runtime), std::move(modelDir));
    }

    ENDPOINT_INFO(listJobs) {
        info->summary = "List AI jobs, with what each is doing";
        info->addResponse<oatpp::List<oatpp::Object<AiJobDto>>>(Status::CODE_200,
                                                                 "application/json");
    }
    ENDPOINT("GET", "/ai-jobs", listJobs) {
        auto jobs = valueOrAbort(m_jobs->list());
        return createDtoResponse(Status::CODE_200, toDtoList(jobs, m_runtime->statuses()));
    }

    ENDPOINT_INFO(listJobsForCamera) {
        info->summary = "The AI jobs on one camera";
        info->addResponse<oatpp::List<oatpp::Object<AiJobDto>>>(Status::CODE_200,
                                                                 "application/json");
    }
    ENDPOINT("GET", "/cameras/{cameraId}/ai-jobs", listJobsForCamera, PATH(String, cameraId)) {
        auto jobs = valueOrAbort(m_jobs->listForCamera(pathId(cameraId)));
        return createDtoResponse(Status::CODE_200, toDtoList(jobs, m_runtime->statuses()));
    }

    ENDPOINT_INFO(getJob) {
        info->summary = "One AI job";
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_200, "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("GET", "/ai-jobs/{id}", getJob, PATH(String, id)) {
        auto job = valueOrAbort(m_jobs->get(pathId(id)));
        return createDtoResponse(Status::CODE_200, toDto(job, statusOf(job.id).get()));
    }

    ENDPOINT_INFO(createJob) {
        info->summary = "Create an AI job";
        info->description =
            "The stage tree is validated here, so a bad job is refused while you are "
            "still looking at it rather than failing to start later.";
        info->addConsumes<oatpp::Object<AiJobDto>>("application/json");
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_201, "application/json");
        info->addResponse(Status::CODE_400, "text/plain");
    }
    ENDPOINT("POST", "/ai-jobs", createJob, BODY_DTO(oatpp::Object<AiJobDto>, body)) {
        auto job = valueOrAbort(m_jobs->create(toChanges(body)));
        return createDtoResponse(Status::CODE_201, toDto(job, statusOf(job.id).get()));
    }

    ENDPOINT_INFO(updateJob) {
        info->summary = "Update an AI job. Omitted fields are left unchanged.";
        info->addConsumes<oatpp::Object<AiJobDto>>("application/json");
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_200, "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("PUT", "/ai-jobs/{id}", updateJob, PATH(String, id),
             BODY_DTO(oatpp::Object<AiJobDto>, body)) {
        auto job = valueOrAbort(m_jobs->update(pathId(id), toChanges(body)));
        return createDtoResponse(Status::CODE_200, toDto(job, statusOf(job.id).get()));
    }

    ENDPOINT_INFO(deleteJob) {
        info->summary = "Delete an AI job";
        info->addResponse(Status::CODE_204, "text/plain");
    }
    ENDPOINT("DELETE", "/ai-jobs/{id}", deleteJob, PATH(String, id)) {
        okOrAbort(m_jobs->remove(pathId(id)));
        return createResponse(Status::CODE_204, "");
    }

    ENDPOINT_INFO(startJob) {
        info->summary = "Start an AI job";
        info->description =
            "This is the `enabled` field, so a job started here stays started across a "
            "restart. A runtime-only flag would not.";
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_200, "application/json");
    }
    ENDPOINT("POST", "/ai-jobs/{id}/start", startJob, PATH(String, id)) {
        auto job = valueOrAbort(m_jobs->setEnabled(pathId(id), true));
        return createDtoResponse(Status::CODE_200, toDto(job, statusOf(job.id).get()));
    }

    ENDPOINT_INFO(stopJob) {
        info->summary = "Stop an AI job";
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_200, "application/json");
    }
    ENDPOINT("POST", "/ai-jobs/{id}/stop", stopJob, PATH(String, id)) {
        auto job = valueOrAbort(m_jobs->setEnabled(pathId(id), false));
        return createDtoResponse(Status::CODE_200, toDto(job, statusOf(job.id).get()));
    }

    // --- the catalogue ---------------------------------------------------------

    ENDPOINT_INFO(listModelTypes) {
        info->summary = "The model types this build knows how to decode";
        info->addResponse<oatpp::List<oatpp::Object<AiCatalogEntryDto>>>(Status::CODE_200,
                                                                          "application/json");
    }
    ENDPOINT("GET", "/ai-model-types", listModelTypes) {
        auto list = oatpp::List<oatpp::Object<AiCatalogEntryDto>>::createShared();
        for (const vision::ModelType* type : vision::modelTypes()) {
            auto dto = AiCatalogEntryDto::createShared();
            dto->id = std::string(type->id());
            dto->label = std::string(type->label());
            dto->description = std::string(type->description());
            list->push_back(dto);
        }
        return createDtoResponse(Status::CODE_200, list);
    }

    ENDPOINT_INFO(listTransforms) {
        info->summary = "The stage transforms available";
        info->addResponse<oatpp::List<oatpp::Object<AiCatalogEntryDto>>>(Status::CODE_200,
                                                                          "application/json");
    }
    ENDPOINT("GET", "/ai-transforms", listTransforms) {
        auto list = oatpp::List<oatpp::Object<AiCatalogEntryDto>>::createShared();
        for (const vision::Transform* item : vision::transforms()) {
            auto dto = AiCatalogEntryDto::createShared();
            dto->id = std::string(item->id());
            dto->label = std::string(item->label());
            dto->description = std::string(item->description());
            list->push_back(dto);
        }
        return createDtoResponse(Status::CODE_200, list);
    }

    ENDPOINT_INFO(listModels) {
        info->summary = "The model files on this machine, and which backend would load each";
        info->addResponse<oatpp::List<oatpp::Object<AiModelDto>>>(Status::CODE_200,
                                                                   "application/json");
    }
    ENDPOINT("GET", "/ai-models", listModels) {
        auto list = oatpp::List<oatpp::Object<AiModelDto>>::createShared();
        std::error_code ec;
        if (!std::filesystem::is_directory(m_modelDir, ec)) {
            // Not an error: a fresh install has no models yet, and an empty
            // list says that better than a 404 does.
            return createDtoResponse(Status::CODE_200, list);
        }
        const auto backends = hal::availableInferenceBackends();
        for (const auto& entry : std::filesystem::directory_iterator(m_modelDir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            auto dto = AiModelDto::createShared();
            dto->path = entry.path().string();
            dto->name = entry.path().filename().string();
            dto->sizeBytes = static_cast<v_uint64>(entry.file_size(ec));
            // Which backend would take it. Absent means nothing here can, which
            // is exactly what an operator needs to know before choosing it.
            for (hal::InferenceBackend* backend : backends) {
                if (backend->handles(hal::ModelRef{dto->path->c_str()})) {
                    dto->backend = std::string(backend->id());
                    break;
                }
            }
            list->push_back(dto);
        }
        return createDtoResponse(Status::CODE_200, list);
    }

private:
    static std::string pathId(const String& id) { return id ? *id : std::string(); }

    std::unique_ptr<media::AiJobStatus> statusOf(const std::string& jobId) const {
        for (const media::AiJobStatus& status : m_runtime->statuses()) {
            if (status.jobId == jobId) {
                return std::make_unique<media::AiJobStatus>(status);
            }
        }
        return nullptr;
    }

    std::shared_ptr<vision::AiJobService> m_jobs;
    std::shared_ptr<media::AiRuntime> m_runtime;
    std::string m_modelDir;
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(ApiController)
