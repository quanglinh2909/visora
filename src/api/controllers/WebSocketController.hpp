#pragma once

// The WebSocket endpoints.
//
// Each does the HTTP upgrade handshake and hands the connection to a
// ConnectionHandler; everything after that is in the feed. The handler arrives
// through the constructor, like every other dependency in this program —
// oatpp's own examples reach for a named OATPP_COMPONENT here, which is how a
// second websocket ends up as a second invisible global.
//
// Not described in Swagger: OpenAPI has no vocabulary for a websocket, and
// listing it as a GET that returns 101 misleads whoever reads the docs.

#include <memory>
#include <utility>

#include "oatpp-websocket/Handshaker.hpp"

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/network/ConnectionHandler.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace visora::api {

class WebSocketController : public oatpp::web::server::api::ApiController {
public:
    WebSocketController(std::shared_ptr<ObjectMapper> objectMapper,
                        std::shared_ptr<oatpp::network::ConnectionHandler> cameraStateHandler,
                        std::shared_ptr<oatpp::network::ConnectionHandler> motionHandler)
        : oatpp::web::server::api::ApiController(std::move(objectMapper)),
          m_cameraStateHandler(std::move(cameraStateHandler)),
          m_motionHandler(std::move(motionHandler)) {}

    static std::shared_ptr<WebSocketController> createShared(
        std::shared_ptr<ObjectMapper> objectMapper,
        std::shared_ptr<oatpp::network::ConnectionHandler> cameraStateHandler,
        std::shared_ptr<oatpp::network::ConnectionHandler> motionHandler) {
        return std::make_shared<WebSocketController>(std::move(objectMapper),
                                                     std::move(cameraStateHandler),
                                                     std::move(motionHandler));
    }

    ENDPOINT("GET", "/ws/camera-state", cameraState,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return oatpp::websocket::Handshaker::serversideHandshake(request->getHeaders(),
                                                                 m_cameraStateHandler);
    }

    // Motion, with the moved cells. A client sends a camera id to receive the
    // per-frame messages for it; events arrive whatever it is watching.
    ENDPOINT("GET", "/ws/motion-events", motionEvents,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return oatpp::websocket::Handshaker::serversideHandshake(request->getHeaders(),
                                                                 m_motionHandler);
    }

private:
    std::shared_ptr<oatpp::network::ConnectionHandler> m_cameraStateHandler;
    std::shared_ptr<oatpp::network::ConnectionHandler> m_motionHandler;
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(ApiController)
