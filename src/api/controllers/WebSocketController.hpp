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
                        std::shared_ptr<oatpp::network::ConnectionHandler> cameraStateHandler)
        : oatpp::web::server::api::ApiController(std::move(objectMapper)),
          m_cameraStateHandler(std::move(cameraStateHandler)) {}

    static std::shared_ptr<WebSocketController> createShared(
        std::shared_ptr<ObjectMapper> objectMapper,
        std::shared_ptr<oatpp::network::ConnectionHandler> cameraStateHandler) {
        return std::make_shared<WebSocketController>(std::move(objectMapper),
                                                     std::move(cameraStateHandler));
    }

    ENDPOINT("GET", "/ws/camera-state", cameraState,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return oatpp::websocket::Handshaker::serversideHandshake(request->getHeaders(),
                                                                 m_cameraStateHandler);
    }

private:
    std::shared_ptr<oatpp::network::ConnectionHandler> m_cameraStateHandler;
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(ApiController)
