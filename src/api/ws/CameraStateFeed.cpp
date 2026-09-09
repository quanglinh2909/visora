#include "api/ws/CameraStateFeed.hpp"

#include <utility>

#include "core/Json.hpp"
#include "core/Log.hpp"

namespace visora::api {
namespace {

constexpr const char* kCategory = "ws";

void appendField(std::string& json, const char* key, std::string_view value, bool first = false) {
    if (!first) json += ',';
    core::appendJsonString(json, key);
    json += ':';
    core::appendJsonString(json, value);
}

}  // namespace

std::string CameraStateFeed::messageFor(const media::CameraRuntimeStatus& status) {
    std::string json = "{";
    appendField(json, "id", status.id, /*first=*/true);
    appendField(json, "name", status.name);
    appendField(json, "state", media::toString(status.state));
    appendField(json, "codec", media::toString(status.codec));
    appendField(json, "outputRtsp", status.outputRtsp);
    appendField(json, "lastError", status.lastError);
    appendField(json, "lastChangedAt", status.lastChangedAt);
    json += ",\"retryCount\":" + std::to_string(status.retryCount < 0 ? 0 : status.retryCount);
    json += ",\"streaming\":";
    json += status.streaming ? "true" : "false";
    json += '}';
    return json;
}

std::string CameraStateFeed::removalMessageFor(const std::string& cameraId) {
    std::string json = "{";
    appendField(json, "id", cameraId, /*first=*/true);
    appendField(json, "state", "removed");
    json += '}';
    return json;
}

void CameraStateFeed::add(const oatpp::websocket::WebSocket* socket) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sockets.insert(socket);
}

void CameraStateFeed::remove(const oatpp::websocket::WebSocket* socket) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sockets.erase(socket);
}

std::size_t CameraStateFeed::clientCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sockets.size();
}

void CameraStateFeed::broadcast(const media::CameraRuntimeStatus& status) {
    if (status.id.empty()) return;

    const std::string message = messageFor(status);
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string& last = m_lastByCamera[status.id];
    if (last == message) return;
    last = message;
    send(message);
}

void CameraStateFeed::broadcastRemoved(const std::string& cameraId) {
    if (cameraId.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastByCamera.erase(cameraId);
    send(removalMessageFor(cameraId));
}

void CameraStateFeed::send(const std::string& message) {
    // Caller holds the lock.
    const oatpp::String frame(message.c_str(), static_cast<v_buff_size>(message.size()));
    for (const auto* socket : m_sockets) {
        try {
            socket->sendOneFrameText(frame);
        } catch (const std::exception& e) {
            // A dead socket is normal — a browser tab closed. Its read loop
            // calls onBeforeDestroy, which unregisters it. Throwing here would
            // take down the streaming worker thread that called us.
            VS_DEBUG(kCategory) << "dropping a frame for a closed socket: " << e.what();
        } catch (...) {
            VS_DEBUG(kCategory) << "dropping a frame for a closed socket";
        }
    }
}

void CameraStateSocketListener::onPing(const WebSocket& socket, const oatpp::String& message) {
    try {
        socket.sendPong(message);
    } catch (...) {
        // Same reasoning as send(): the socket is gone, and it is not our job
        // to notice here.
    }
}

CameraStateInstanceListener::CameraStateInstanceListener(std::shared_ptr<CameraStateFeed> feed)
    : m_feed(std::move(feed)),
      m_socketListener(std::make_shared<CameraStateSocketListener>()) {}

void CameraStateInstanceListener::onAfterCreate(const WebSocket& socket,
                                                const std::shared_ptr<const ParameterMap>&) {
    socket.setListener(m_socketListener);
    m_feed->add(&socket);
    VS_DEBUG(kCategory) << "camera-state client connected (" << m_feed->clientCount() << ")";
}

void CameraStateInstanceListener::onBeforeDestroy(const WebSocket& socket) {
    m_feed->remove(&socket);
}

}  // namespace visora::api
