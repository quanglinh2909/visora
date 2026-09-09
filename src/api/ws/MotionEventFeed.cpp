#include "api/ws/MotionEventFeed.hpp"

#include <sstream>
#include <utility>
#include <vector>

#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/Time.hpp"

namespace visora::api {
namespace {

constexpr const char* kCategory = "ws";

void appendField(std::string& json, const char* key, std::string_view value, bool first) {
    if (!first) json += ',';
    core::appendJsonString(json, key);
    json += ':';
    core::appendJsonString(json, value);
}

}  // namespace

std::string MotionEventFeed::frameMessage(const media::MotionNotice& notice,
                                          const std::string& inside,
                                          const std::string& outside) {
    std::string json = "{";
    appendField(json, "type", "frame", /*first=*/true);
    appendField(json, "cameraId", notice.cameraId, false);
    appendField(json, "inside", inside, false);
    appendField(json, "outside", outside, false);
    json += ",\"gridX\":" + std::to_string(notice.gridX);
    json += ",\"gridY\":" + std::to_string(notice.gridY);
    json += ",\"triggered\":";
    json += notice.triggered ? "true" : "false";
    json += '}';
    return json;
}

std::string MotionEventFeed::eventMessage(const std::string& cameraId, bool starting,
                                          const std::string& cells) {
    std::string json = "{";
    appendField(json, "type", "event", /*first=*/true);
    appendField(json, "cameraId", cameraId, false);
    appendField(json, "state", starting ? "start" : "end", false);
    appendField(json, "cells", cells, false);
    appendField(json, "at", core::nowIso8601(), false);
    json += '}';
    return json;
}

void MotionEventFeed::add(const oatpp::websocket::WebSocket* socket) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sockets[socket] = std::string();
}

void MotionEventFeed::remove(const oatpp::websocket::WebSocket* socket) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sockets.erase(socket);
}

void MotionEventFeed::subscribe(const oatpp::websocket::WebSocket* socket,
                                const std::string& cameraId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sockets.find(socket);
    if (it == m_sockets.end()) return;
    it->second = cameraId;
    VS_DEBUG(kCategory) << "motion client now watching "
                        << (cameraId.empty() ? "nothing" : cameraId);
}

std::size_t MotionEventFeed::clientCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sockets.size();
}

void MotionEventFeed::publish(const media::MotionNotice& notice) {
    if (notice.cameraId.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    // The event edge first: a start once when a zone fires, an end once when it
    // stops. A client that only cares about events gets two messages per event
    // rather than five a second.
    bool& open = m_eventOpen[notice.cameraId];
    if (notice.triggered != open) {
        open = notice.triggered;
        send(eventMessage(notice.cameraId, notice.triggered, notice.cells), notice.cameraId,
             /*onlySubscribers=*/false);
    }

    // Frame messages only to clients watching this camera; see the header.
    bool anySubscriber = false;
    for (const auto& [socket, cameraId] : m_sockets) {
        if (cameraId == notice.cameraId) {
            anySubscriber = true;
            break;
        }
    }
    if (!anySubscriber) return;

    // Already split by the runtime, which is where the zones are.
    send(frameMessage(notice, notice.insideCells, notice.outsideCells), notice.cameraId,
         /*onlySubscribers=*/true);
}

void MotionEventFeed::send(const std::string& message, const std::string& cameraId,
                           bool onlySubscribers) {
    // Caller holds the lock.
    const oatpp::String frame(message.c_str(), static_cast<v_buff_size>(message.size()));
    for (const auto& [socket, watching] : m_sockets) {
        if (onlySubscribers && watching != cameraId) continue;
        try {
            socket->sendOneFrameText(frame);
        } catch (...) {
            // A closed tab. Its read loop unregisters it; throwing here would
            // take down the decoder thread that called us.
        }
    }
}

MotionSocketListener::MotionSocketListener(std::shared_ptr<MotionEventFeed> feed)
    : m_feed(std::move(feed)) {}

void MotionSocketListener::onPing(const WebSocket& socket, const oatpp::String& message) {
    try {
        socket.sendPong(message);
    } catch (...) {
    }
}

void MotionSocketListener::readMessage(const WebSocket& socket, v_uint8, p_char8 data,
                                       oatpp::v_io_size size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string& buffer = m_partial[&socket];
    if (size > 0 && data != nullptr) {
        buffer.append(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size));
        return;
    }
    // size == 0 marks the end of a message.
    std::string cameraId;
    cameraId.swap(buffer);
    // Trim, because a client sending a line will include the newline.
    while (!cameraId.empty() &&
           (cameraId.back() == '\n' || cameraId.back() == '\r' || cameraId.back() == ' ')) {
        cameraId.pop_back();
    }
    m_feed->subscribe(&socket, cameraId);
}

MotionInstanceListener::MotionInstanceListener(std::shared_ptr<MotionEventFeed> feed)
    : m_feed(std::move(feed)),
      m_socketListener(std::make_shared<MotionSocketListener>(m_feed)) {}

void MotionInstanceListener::onAfterCreate(const WebSocket& socket,
                                           const std::shared_ptr<const ParameterMap>&) {
    socket.setListener(m_socketListener);
    m_feed->add(&socket);
}

void MotionInstanceListener::onBeforeDestroy(const WebSocket& socket) {
    m_feed->remove(&socket);
}

}  // namespace visora::api
