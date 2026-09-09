#pragma once

// MoQ feeds.
//
// Called by the MoQ server, not by a browser: it opens a feed when a viewer
// subscribes and closes it when the last one leaves. So there is no CORS here
// and no SDP — just a small control API between two server processes.

#include <memory>
#include <string>
#include <utility>

#include "api/HttpError.hpp"
#include "api/dto/MoqDto.hpp"
#include "core/Time.hpp"
#include "media/moq/MoqService.hpp"

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace visora::api {

class MoqController : public oatpp::web::server::api::ApiController {
public:
    MoqController(std::shared_ptr<ObjectMapper> objectMapper,
                  std::shared_ptr<media::MoqService> moq)
        : oatpp::web::server::api::ApiController(std::move(objectMapper)),
          m_moq(std::move(moq)) {}

    static std::shared_ptr<MoqController> createShared(
        std::shared_ptr<ObjectMapper> objectMapper, std::shared_ptr<media::MoqService> moq) {
        return std::make_shared<MoqController>(std::move(objectMapper), std::move(moq));
    }

    ENDPOINT_INFO(createFeed) {
        info->summary = "Open a MoQ frame feed (called by the MoQ server)";
        info->description =
            "mode=live attaches to the camera's shared source; mode=playback opens a "
            "playback source reading recordings from 'at'.";
        info->addConsumes<oatpp::Object<MoqFeedRequestDto>>("application/json");
        info->addResponse<oatpp::Object<MoqFeedDto>>(Status::CODE_201, "application/json");
        info->addResponse(Status::CODE_400, "text/plain");
        info->addResponse(Status::CODE_503, "text/plain");
    }
    ENDPOINT("POST", "/moq/feeds", createFeed,
             BODY_DTO(oatpp::Object<MoqFeedRequestDto>, body)) {
        media::MoqFeedRequest request;
        request.feedId = body && body->feed ? std::string(body->feed->c_str()) : std::string();
        request.cameraId =
            body && body->cameraId ? std::string(body->cameraId->c_str()) : std::string();
        request.mode = body && body->mode ? std::string(body->mode->c_str()) : "live";
        if (body && body->at && !body->at->empty()) {
            request.atMs = core::parseEpochMs(body->at->c_str());
            if (request.atMs < 0) abortWith(core::invalidArgument("'at' is not a timestamp"));
        } else {
            request.atMs = core::nowEpochMs();
        }

        return createDtoResponse(Status::CODE_201, toDto(valueOrAbort(m_moq->open(request))));
    }

    ENDPOINT_INFO(deleteFeed) {
        info->summary = "Close a MoQ frame feed";
        info->addResponse(Status::CODE_204, "text/plain");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("DELETE", "/moq/feeds/{sessionId}", deleteFeed, PATH(String, sessionId)) {
        okOrAbort(m_moq->close(sessionId ? *sessionId : std::string()));
        return createResponse(Status::CODE_204, "");
    }

    ENDPOINT_INFO(listFeeds) {
        info->summary = "The MoQ feeds currently open";
        info->addResponse<oatpp::List<oatpp::Object<MoqFeedDto>>>(Status::CODE_200,
                                                                   "application/json");
    }
    ENDPOINT("GET", "/moq/feeds", listFeeds, QUERY(String, cameraId, "cameraId", "")) {
        auto list = oatpp::List<oatpp::Object<MoqFeedDto>>::createShared();
        for (const auto& feed :
             m_moq->feeds(cameraId ? std::string(cameraId->c_str()) : std::string())) {
            list->push_back(toDto(feed));
        }
        return createDtoResponse(Status::CODE_200, list);
    }

private:
    static oatpp::Object<MoqFeedDto> toDto(const media::MoqFeedInfo& info) {
        auto dto = MoqFeedDto::createShared();
        dto->sessionId = info.sessionId;
        dto->feed = info.feedId;
        dto->cameraId = info.cameraId;
        dto->mode = info.mode;
        dto->framesSent = info.framesSent;
        dto->framesDropped = info.framesDropped;
        return dto;
    }

    std::shared_ptr<media::MoqService> m_moq;
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(ApiController)
