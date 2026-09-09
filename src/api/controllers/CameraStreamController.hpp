#pragma once

// Live-stream endpoints: what a camera is doing now, and the three verbs that
// change it.
//
// Separate from CameraController because they are a different resource with a
// different lifetime — one is the stored configuration, the other is the
// running process. Splitting them also keeps each controller small enough to
// read in one screen, which the predecessor's 420-line camera controller was
// not.

#include <memory>
#include <string>
#include <utility>

#include "api/HttpError.hpp"
#include "api/mappers/StreamStatusMapper.hpp"
#include "media/camera/CameraRuntime.hpp"

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace visora::api {

class CameraStreamController : public oatpp::web::server::api::ApiController {
public:
    CameraStreamController(std::shared_ptr<ObjectMapper> objectMapper,
                           std::shared_ptr<media::CameraRuntime> runtime)
        : oatpp::web::server::api::ApiController(std::move(objectMapper)),
          m_runtime(std::move(runtime)) {}

    static std::shared_ptr<CameraStreamController> createShared(
        std::shared_ptr<ObjectMapper> objectMapper,
        std::shared_ptr<media::CameraRuntime> runtime) {
        return std::make_shared<CameraStreamController>(std::move(objectMapper),
                                                        std::move(runtime));
    }

    ENDPOINT_INFO(listStreams) {
        info->summary = "Live stream status for every camera";
        info->addResponse<oatpp::List<oatpp::Object<StreamStatusDto>>>(Status::CODE_200,
                                                                       "application/json");
    }
    ENDPOINT("GET", "/camera-streams", listStreams) {
        return createDtoResponse(Status::CODE_200,
                                 toDtoList(valueOrAbort(m_runtime->statuses())));
    }

    ENDPOINT_INFO(getStream) {
        info->summary = "Live stream status for one camera";
        info->addResponse<oatpp::Object<StreamStatusDto>>(Status::CODE_200, "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("GET", "/cameras/{id}/stream", getStream, PATH(String, id)) {
        return createDtoResponse(Status::CODE_200,
                                 toDto(valueOrAbort(m_runtime->statusOf(pathId(id)))));
    }

    ENDPOINT_INFO(startStream) {
        info->summary =
            "Start streaming a camera. Returns immediately: connecting takes seconds, "
            "so follow /ws/camera-state for the outcome.";
        info->addResponse<oatpp::Object<StreamStatusDto>>(Status::CODE_202, "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("POST", "/cameras/{id}/stream/start", startStream, PATH(String, id)) {
        return createDtoResponse(Status::CODE_202,
                                 toDto(valueOrAbort(m_runtime->start(pathId(id)))));
    }

    ENDPOINT_INFO(stopStream) {
        info->summary = "Stop streaming a camera. It stays stopped until started again.";
        info->addResponse<oatpp::Object<StreamStatusDto>>(Status::CODE_200, "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("POST", "/cameras/{id}/stream/stop", stopStream, PATH(String, id)) {
        return createDtoResponse(Status::CODE_200,
                                 toDto(valueOrAbort(m_runtime->stop(pathId(id)))));
    }

    ENDPOINT_INFO(restartStream) {
        info->summary = "Tear the pipeline down and rebuild it";
        info->addResponse<oatpp::Object<StreamStatusDto>>(Status::CODE_202, "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("POST", "/cameras/{id}/stream/restart", restartStream, PATH(String, id)) {
        return createDtoResponse(Status::CODE_202,
                                 toDto(valueOrAbort(m_runtime->restart(pathId(id)))));
    }

    ENDPOINT_INFO(getSnapshot) {
        info->summary = "A JPEG of what the camera sees now";
        info->addResponse<String>(Status::CODE_200, "image/jpeg");
        info->addResponse(Status::CODE_404, "text/plain");
        info->addResponse(Status::CODE_503, "text/plain");
    }
    ENDPOINT("GET", "/cameras/{id}/snapshot", getSnapshot, PATH(String, id)) {
        const auto jpeg = valueOrAbort(m_runtime->snapshot(pathId(id)));
        auto response = createResponse(
            Status::CODE_200, oatpp::String(reinterpret_cast<const char*>(jpeg.data()),
                                            static_cast<v_buff_size>(jpeg.size())));
        response->putHeader(Header::CONTENT_TYPE, "image/jpeg");
        // A snapshot is the current moment by definition; a cached one is a
        // lie, and browsers will happily serve one for an <img> that is polled.
        response->putHeader("Cache-Control", "no-store");
        return response;
    }

private:
    static std::string pathId(const String& id) { return id ? *id : std::string(); }

    std::shared_ptr<media::CameraRuntime> m_runtime;
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(ApiController)
