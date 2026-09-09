#pragma once

// The camera endpoints.
//
// A thin adapter, by design: parse, delegate, map, return. There is no business
// logic here and no database access — those live in CameraService and in a
// repository behind a port, which is why both are tested without an HTTP server
// or a database.
//
// The service arrives through the constructor rather than through
// OATPP_COMPONENT. A service locator hides what a class needs, makes the
// dependency graph invisible, and forces a test to construct the whole
// application.

#include <memory>
#include <utility>

#include "api/HttpError.hpp"
#include "api/mappers/CameraMapper.hpp"
#include "media/camera/CameraService.hpp"

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace visora::api {

class CameraController : public oatpp::web::server::api::ApiController {
public:
    CameraController(std::shared_ptr<ObjectMapper> objectMapper,
                     std::shared_ptr<media::CameraService> cameras)
        : oatpp::web::server::api::ApiController(std::move(objectMapper)),
          m_cameras(std::move(cameras)) {}

    static std::shared_ptr<CameraController> createShared(
        std::shared_ptr<ObjectMapper> objectMapper,
        std::shared_ptr<media::CameraService> cameras) {
        return std::make_shared<CameraController>(std::move(objectMapper), std::move(cameras));
    }

    ENDPOINT_INFO(listCameras) {
        info->summary = "List cameras";
        info->addResponse<oatpp::List<oatpp::Object<CameraDto>>>(Status::CODE_200,
                                                                 "application/json");
    }
    ENDPOINT("GET", "/cameras", listCameras) {
        auto cameras = valueOrAbort(m_cameras->list());
        return createDtoResponse(Status::CODE_200, toDtoList(cameras));
    }

    ENDPOINT_INFO(getCamera) {
        info->summary = "Get one camera";
        info->addResponse<oatpp::Object<CameraDto>>(Status::CODE_200, "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("GET", "/cameras/{id}", getCamera, PATH(String, id)) {
        auto camera = valueOrAbort(m_cameras->get(id ? *id : std::string()));
        return createDtoResponse(Status::CODE_200, toDto(camera));
    }

    ENDPOINT_INFO(createCamera) {
        info->summary = "Create a camera";
        info->addConsumes<oatpp::Object<CameraDto>>("application/json");
        info->addResponse<oatpp::Object<CameraDto>>(Status::CODE_201, "application/json");
        info->addResponse(Status::CODE_400, "text/plain");
    }
    ENDPOINT("POST", "/cameras", createCamera, BODY_DTO(oatpp::Object<CameraDto>, body)) {
        auto camera = valueOrAbort(m_cameras->create(toChanges(body)));
        return createDtoResponse(Status::CODE_201, toDto(camera));
    }

    ENDPOINT_INFO(updateCamera) {
        info->summary = "Update a camera. Omitted fields are left unchanged.";
        info->addConsumes<oatpp::Object<CameraDto>>("application/json");
        info->addResponse<oatpp::Object<CameraDto>>(Status::CODE_200, "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("PUT", "/cameras/{id}", updateCamera, PATH(String, id),
             BODY_DTO(oatpp::Object<CameraDto>, body)) {
        auto camera =
            valueOrAbort(m_cameras->update(id ? *id : std::string(), toChanges(body)));
        return createDtoResponse(Status::CODE_200, toDto(camera));
    }

    ENDPOINT_INFO(deleteCamera) {
        info->summary = "Delete a camera";
        info->addResponse(Status::CODE_204, "text/plain");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("DELETE", "/cameras/{id}", deleteCamera, PATH(String, id)) {
        okOrAbort(m_cameras->remove(id ? *id : std::string()));
        return createResponse(Status::CODE_204, "");
    }

private:
    std::shared_ptr<media::CameraService> m_cameras;
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(ApiController)
