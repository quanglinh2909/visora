#pragma once

// WHEP: WebRTC-HTTP Egress.
//
// The whole protocol is one POST of an SDP offer answered with an SDP answer,
// plus a DELETE to hang up. Content types are `application/sdp` in both
// directions and the answer carries a Location header — that is what makes an
// off-the-shelf WHEP client work against this without changes.
//
// The OPTIONS endpoints exist for CORS preflight: a browser page served from
// anywhere but this server will not send the POST without them.

#include <memory>
#include <string>
#include <utility>

#include "api/HttpError.hpp"
#include "api/dto/WebRtcDto.hpp"
#include "core/Time.hpp"
#include "media/webrtc/WebRtcService.hpp"

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace visora::api {

class WebRtcController : public oatpp::web::server::api::ApiController {
public:
    WebRtcController(std::shared_ptr<ObjectMapper> objectMapper,
                     std::shared_ptr<media::WebRtcService> webrtc)
        : oatpp::web::server::api::ApiController(std::move(objectMapper)),
          m_webrtc(std::move(webrtc)) {}

    static std::shared_ptr<WebRtcController> createShared(
        std::shared_ptr<ObjectMapper> objectMapper,
        std::shared_ptr<media::WebRtcService> webrtc) {
        return std::make_shared<WebRtcController>(std::move(objectMapper), std::move(webrtc));
    }

    ENDPOINT_INFO(whepOffer) {
        info->summary = "Start watching a camera over WebRTC (WHEP)";
        info->addConsumes<String>("application/sdp");
        info->addResponse<String>(Status::CODE_201, "application/sdp");
        info->addResponse(Status::CODE_400, "text/plain");
        info->addResponse(Status::CODE_404, "text/plain");
        info->addResponse(Status::CODE_503, "text/plain");
    }
    ENDPOINT("POST", "/cameras/{id}/whep", whepOffer, PATH(String, id),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        media::WhepOffer offer;
        offer.cameraId = id ? *id : std::string();
        const auto body = request->readBodyToString();
        offer.sdp = body ? std::string(body->c_str(), body->size()) : std::string();
        offer.clientAddress = clientAddressOf(request);

        const auto answer = valueOrAbort(m_webrtc->offer(offer));

        // 201 with the answer as the body, per WHEP.
        auto response = createResponse(Status::CODE_201, answer.sdp.c_str());
        response->putHeader(Header::CONTENT_TYPE, "application/sdp");
        // Where to DELETE this session. A spec-following client uses it rather
        // than constructing the URL itself.
        response->putHeader("Location", answer.location.c_str());
        addCors(response);
        return response;
    }

    ENDPOINT_INFO(whepStop) {
        info->summary = "Stop a WebRTC session";
        info->addResponse(Status::CODE_204, "text/plain");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("DELETE", "/cameras/{id}/whep/{sessionId}", whepStop, PATH(String, id),
             PATH(String, sessionId)) {
        okOrAbort(m_webrtc->close(sessionId ? *sessionId : std::string()));
        auto response = createResponse(Status::CODE_204, "");
        addCors(response);
        return response;
    }

    ENDPOINT_INFO(whepViewers) {
        info->summary = "Who is watching over WebRTC right now";
        info->addResponse<oatpp::List<oatpp::Object<ViewerDto>>>(Status::CODE_200,
                                                                 "application/json");
    }
    ENDPOINT("GET", "/webrtc/viewers", whepViewers) {
        auto list = oatpp::List<oatpp::Object<ViewerDto>>::createShared();
        for (const auto& viewer : m_webrtc->viewers()) {
            auto dto = ViewerDto::createShared();
            dto->sessionId = viewer.sessionId;
            dto->cameraId = viewer.cameraId;
            dto->codec = viewer.codec;
            dto->transcoded = viewer.transcoded;
            dto->rtpPackets = viewer.rtpPackets;
            dto->startedAt = core::toIso8601(viewer.startedAtMs);
            list->push_back(dto);
        }
        return createDtoResponse(Status::CODE_200, list);
    }

    // CORS preflight. Without these a page served from anywhere but this origin
    // never gets to send the POST at all.
    ENDPOINT("OPTIONS", "/cameras/{id}/whep", whepOptions, PATH(String, id)) {
        return preflight();
    }
    ENDPOINT("OPTIONS", "/cameras/{id}/whep/{sessionId}", whepSessionOptions, PATH(String, id),
             PATH(String, sessionId)) {
        return preflight();
    }

private:
    // The address the request came from, preferring the proxy's own record of
    // it. Only ever used to substitute for an unresolvable mDNS name in an ICE
    // candidate, so a wrong guess costs one candidate, not the session.
    static std::string clientAddressOf(const std::shared_ptr<IncomingRequest>& request) {
        const auto forwarded = request->getHeader("X-Forwarded-For");
        if (!forwarded) return {};
        const std::string value(forwarded->c_str(), forwarded->size());
        // The first entry is the original client; the rest are proxies.
        const auto comma = value.find(',');
        std::string first = comma == std::string::npos ? value : value.substr(0, comma);
        while (!first.empty() && (first.front() == ' ' || first.front() == '\t')) {
            first.erase(first.begin());
        }
        while (!first.empty() && (first.back() == ' ' || first.back() == '\t')) {
            first.pop_back();
        }
        return first;
    }

    template <class Response>
    static void addCors(const Response& response) {
        response->putHeader("Access-Control-Allow-Origin", "*");
        response->putHeader("Access-Control-Expose-Headers", "Location");
    }

    std::shared_ptr<OutgoingResponse> preflight() {
        auto response = createResponse(Status::CODE_204, "");
        addCors(response);
        response->putHeader("Access-Control-Allow-Methods", "POST, DELETE, OPTIONS");
        response->putHeader("Access-Control-Allow-Headers", "Content-Type");
        response->putHeader("Access-Control-Max-Age", "86400");
        return response;
    }

    std::shared_ptr<media::WebRtcService> m_webrtc;
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(ApiController)
