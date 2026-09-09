#pragma once

// Push of camera state changes to browsers.
//
//   GET /ws/camera-state
//
// Server -> client only. Each message is one camera:
//
//   {"id":"...","name":"...","state":"online|offline|error",
//    "codec":"h264","outputRtsp":"rtsp://...","retryCount":0,
//    "lastError":"","lastChangedAt":"..."}
//
// The shape is the predecessor's plus the fields a dashboard was already
// fetching separately; a client that only reads id/state/lastError is
// unaffected.
//
// The JSON is written by hand rather than through the object mapper. It is a
// fixed four-field contract that a deployed UI parses, and building an oatpp
// DTO and a serializer per broadcast — potentially per camera per second on a
// large install — buys nothing. core::appendJsonString does the part that is
// easy to get wrong.

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "media/camera/CameraRuntime.hpp"

#include "oatpp-websocket/ConnectionHandler.hpp"
#include "oatpp-websocket/WebSocket.hpp"

namespace visora::api {

// The connected sockets, and the fan-out.
//
// Called from the streaming layer's worker thread, so every method locks. The
// same lock serialises broadcasts, because two threads writing one websocket
// interleave frames and corrupt the stream.
class CameraStateFeed {
public:
    void add(const oatpp::websocket::WebSocket* socket);
    void remove(const oatpp::websocket::WebSocket* socket);

    // Sends one message to every client, but only when something a client can
    // see actually changed. The status callback fires on transitions that map
    // to the same visible state — a retry that fails the same way as the last
    // one — and a UI does not need to be told twice.
    void broadcast(const media::CameraRuntimeStatus& status);

    // A camera that no longer exists. Clients remove the row; we forget its
    // last message so a camera re-added with the same id is not deduplicated
    // against a state from its previous life.
    void broadcastRemoved(const std::string& cameraId);

    std::size_t clientCount() const;

    // Exposed for tests: the exact bytes a client would receive.
    static std::string messageFor(const media::CameraRuntimeStatus& status);
    static std::string removalMessageFor(const std::string& cameraId);

private:
    void send(const std::string& message);

    mutable std::mutex m_mutex;
    std::set<const oatpp::websocket::WebSocket*> m_sockets;
    // Last message sent per camera, for the change check above.
    std::map<std::string, std::string> m_lastByCamera;
};

// Per-socket listener. This feed is server -> client, so inbound frames are
// ignored; ping is answered, because a proxy that sees no traffic closes an
// idle websocket after a minute or two.
class CameraStateSocketListener : public oatpp::websocket::WebSocket::Listener {
public:
    void onPing(const WebSocket& socket, const oatpp::String& message) override;
    void onPong(const WebSocket&, const oatpp::String&) override {}
    void onClose(const WebSocket&, v_uint16, const oatpp::String&) override {}
    void readMessage(const WebSocket&, v_uint8, p_char8, oatpp::v_io_size) override {}
};

// Registers each accepted socket with the feed and unregisters it on close.
class CameraStateInstanceListener
    : public oatpp::websocket::ConnectionHandler::SocketInstanceListener {
public:
    explicit CameraStateInstanceListener(std::shared_ptr<CameraStateFeed> feed);

    void onAfterCreate(const WebSocket& socket,
                       const std::shared_ptr<const ParameterMap>&) override;
    void onBeforeDestroy(const WebSocket& socket) override;

private:
    std::shared_ptr<CameraStateFeed> m_feed;
    std::shared_ptr<CameraStateSocketListener> m_socketListener;
};

}  // namespace visora::api
