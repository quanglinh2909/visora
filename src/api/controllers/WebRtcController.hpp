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

#include <algorithm>
#include <cstdint>
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
    ENDPOINT("GET", "/webrtc/viewers", whepViewers,
             QUERY(String, cameraId, "cameraId", "")) {
        const std::string filter = cameraId ? cameraId->c_str() : "";
        auto list = oatpp::List<oatpp::Object<ViewerDto>>::createShared();
        const std::int64_t now = core::nowEpochMs();
        std::int64_t live = 0;
        std::int64_t playback = 0;

        for (const auto& viewer : m_webrtc->viewers()) {
            if (!filter.empty() && viewer.cameraId != filter) continue;
            if (viewer.playback) ++playback; else ++live;

            auto dto = ViewerDto::createShared();
            dto->sessionId = viewer.sessionId;
            dto->cameraId = viewer.cameraId;
            dto->codec = viewer.codec;
            dto->transcoded = viewer.transcoded;
            dto->rtpPackets = viewer.rtpPackets;
            dto->startedAt = core::toIso8601(viewer.startedAtMs);
            dto->clientAddr = viewer.clientAddr;
            dto->mode = viewer.playback ? "playback" : "live";
            dto->connected = viewer.connected;
            const std::int64_t ageMs =
                viewer.startedAtMs > 0 ? std::max<std::int64_t>(0, now - viewer.startedAtMs) : 0;
            dto->ageMs = ageMs;
            dto->ageSeconds = ageMs / 1000;
            list->push_back(dto);
        }

        auto out = ViewersDto::createShared();
        out->total = live + playback;
        out->live = live;
        out->playback = playback;
        out->sessions = list;
        return createDtoResponse(Status::CODE_200, out);
    }

    // --- watching a recording -------------------------------------------------

    ENDPOINT_INFO(playbackOffer) {
        info->summary = "Start watching a recording over WebRTC";
        info->description =
            "Opens ONE session for the whole timeline. Every later click is a command "
            "to /playback/{sessionId}/control rather than a new connection — which is "
            "what makes scrubbing cost the same however much has been recorded.";
        info->addConsumes<String>("application/sdp");
        info->addResponse<String>(Status::CODE_201, "application/sdp");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("POST", "/cameras/{id}/playback/whep", playbackOffer, PATH(String, id),
             QUERY(String, at, "at", ""),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        media::PlaybackOffer offer;
        offer.cameraId = id ? *id : std::string();
        const auto body = request->readBodyToString();
        offer.sdp = body ? std::string(body->c_str(), body->size()) : std::string();
        offer.clientAddress = clientAddressOf(request);
        offer.atMs = at && !at->empty() ? core::parseEpochMs(*at) : core::nowEpochMs();
        if (offer.atMs < 0) abortWith(core::invalidArgument("'at' is not a timestamp"));

        const auto answer = valueOrAbort(m_webrtc->offerPlayback(offer));
        auto response = createResponse(Status::CODE_201, answer.sdp.c_str());
        response->putHeader(Header::CONTENT_TYPE, "application/sdp");
        response->putHeader("Location", answer.location.c_str());
        addCors(response);
        return response;
    }

    ENDPOINT_INFO(playbackControl) {
        info->summary = "Seek, pause, resume or change speed";
        info->addConsumes<oatpp::Object<PlaybackControlDto>>("application/json");
        info->addResponse<oatpp::Object<PlaybackStatusDto>>(Status::CODE_200,
                                                            "application/json");
        info->addResponse(Status::CODE_400, "text/plain");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("POST", "/playback/{sessionId}/control", playbackControl, PATH(String, sessionId),
             BODY_DTO(oatpp::Object<PlaybackControlDto>, body)) {
        const std::string id = sessionId ? *sessionId : std::string();
        okOrAbort(m_webrtc->control(id, toCommand(body)));
        return createDtoResponse(Status::CODE_200,
                                 toStatus(id, valueOrAbort(m_webrtc->playbackState(id))));
    }

    ENDPOINT_INFO(playbackStatus) {
        info->summary = "Where a playback session has got to";
        info->addResponse<oatpp::Object<PlaybackStatusDto>>(Status::CODE_200,
                                                            "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("GET", "/playback/{sessionId}", playbackStatus, PATH(String, sessionId)) {
        const std::string id = sessionId ? *sessionId : std::string();
        return createDtoResponse(Status::CODE_200,
                                 toStatus(id, valueOrAbort(m_webrtc->playbackState(id))));
    }

    ENDPOINT_INFO(playbackStop) {
        info->summary = "End a playback session";
        info->addResponse(Status::CODE_204, "text/plain");
    }
    ENDPOINT("DELETE", "/playback/{sessionId}", playbackStop, PATH(String, sessionId)) {
        okOrAbort(m_webrtc->close(sessionId ? *sessionId : std::string()));
        auto response = createResponse(Status::CODE_204, "");
        addCors(response);
        return response;
    }

    ENDPOINT("OPTIONS", "/cameras/{id}/playback/whep", playbackOfferOptions, PATH(String, id)) {
        return preflight();
    }
    ENDPOINT("OPTIONS", "/playback/{sessionId}", playbackStopOptions, PATH(String, sessionId)) {
        return preflight();
    }
    ENDPOINT("OPTIONS", "/playback/{sessionId}/control", playbackControlOptions,
             PATH(String, sessionId)) {
        return preflight();
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
    static media::PlaybackCommand toCommand(const oatpp::Object<PlaybackControlDto>& dto) {
        media::PlaybackCommand command;
        const std::string action =
            dto && dto->action ? std::string(dto->action->c_str()) : std::string("seek");
        if (action == "pause") {
            command.action = media::PlaybackCommand::Action::Pause;
        } else if (action == "resume") {
            command.action = media::PlaybackCommand::Action::Resume;
        } else if (action == "rate") {
            command.action = media::PlaybackCommand::Action::Rate;
            command.rate = dto && dto->rate ? *dto->rate : 1.0;
        } else if (action == "seek") {
            command.action = media::PlaybackCommand::Action::Seek;
            command.atMs = dto && dto->at ? core::parseEpochMs(dto->at->c_str())
                                          : core::nowEpochMs();
            if (command.atMs < 0) {
                abortWith(core::invalidArgument("'at' is not a timestamp"));
            }
        } else {
            abortWith(core::invalidArgument("unknown action: " + action));
        }
        return command;
    }

    static oatpp::Object<PlaybackStatusDto> toStatus(const std::string& sessionId,
                                                     const media::PlaybackState& state) {
        auto dto = PlaybackStatusDto::createShared();
        dto->sessionId = sessionId;
        dto->position = core::toIso8601Millis(state.positionMs);
        dto->rate = state.rate;
        dto->paused = state.paused;
        dto->ended = state.ended;
        return dto;
    }

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
