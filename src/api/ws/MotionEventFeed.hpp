#pragma once

// Push of motion to browsers.
//
//   GET /ws/motion-events
//
// TWO kinds of message, and the distinction is the whole design:
//
//   {"type":"frame","cameraId":"...","inside":"3:4,3:5","outside":"9:1", ...}
//     every analysed frame, ~5 a second, so a live view can highlight what is
//     moving RIGHT NOW — including movement outside every drawn zone, drawn in
//     a different colour, which is how an operator discovers a zone is in the
//     wrong place.
//
//   {"type":"event","cameraId":"...","state":"start"|"end", ...}
//     only when a zone's level is reached, which is what gets recorded.
//
// Frame messages go ONLY to clients that subscribed to that camera. Sixteen
// cameras at five frames a second broadcast to everyone is 80 messages a second
// of which each viewer wants five.

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "media/ai/AiRuntime.hpp"

#include "oatpp-websocket/ConnectionHandler.hpp"
#include "oatpp-websocket/WebSocket.hpp"

namespace visora::api {

class MotionEventFeed {
public:
    void add(const oatpp::websocket::WebSocket* socket);
    void remove(const oatpp::websocket::WebSocket* socket);

    // A client says which camera it is looking at. Sent as a plain camera id,
    // or empty to stop receiving frame messages.
    void subscribe(const oatpp::websocket::WebSocket* socket, const std::string& cameraId);

    // Called for every analysed frame. Sends a frame message to subscribers,
    // and an event message to EVERYONE when a zone's level is crossed — an
    // event matters whatever a client is currently looking at.
    void publish(const media::MotionNotice& notice);

    std::size_t clientCount() const;

    // Exposed for tests: the exact bytes a client would receive.
    static std::string frameMessage(const media::MotionNotice& notice,
                                    const std::string& inside, const std::string& outside);
    static std::string eventMessage(const std::string& cameraId, bool starting,
                                    const std::string& cells);

private:
    void send(const std::string& message, const std::string& cameraId, bool onlySubscribers);

    mutable std::mutex m_mutex;
    std::map<const oatpp::websocket::WebSocket*, std::string> m_sockets;  // -> camera id
    // Whether each camera currently has an event open, so a start is sent once
    // and an end is sent when movement stops.
    std::map<std::string, bool> m_eventOpen;
};

class MotionSocketListener : public oatpp::websocket::WebSocket::Listener {
public:
    MotionSocketListener(std::shared_ptr<MotionEventFeed> feed);

    void onPing(const WebSocket& socket, const oatpp::String& message) override;
    void onPong(const WebSocket&, const oatpp::String&) override {}
    void onClose(const WebSocket&, v_uint16, const oatpp::String&) override {}
    // The one thing a client may say: which camera to watch.
    void readMessage(const WebSocket& socket, v_uint8 opcode, p_char8 data,
                     oatpp::v_io_size size) override;

private:
    std::shared_ptr<MotionEventFeed> m_feed;
    // Message fragments arrive separately; a subscription is short enough that
    // gathering them per socket is simpler than streaming.
    std::mutex m_mutex;
    std::map<const WebSocket*, std::string> m_partial;
};

class MotionInstanceListener
    : public oatpp::websocket::ConnectionHandler::SocketInstanceListener {
public:
    explicit MotionInstanceListener(std::shared_ptr<MotionEventFeed> feed);

    void onAfterCreate(const WebSocket& socket,
                       const std::shared_ptr<const ParameterMap>&) override;
    void onBeforeDestroy(const WebSocket& socket) override;

private:
    std::shared_ptr<MotionEventFeed> m_feed;
    std::shared_ptr<MotionSocketListener> m_socketListener;
};

}  // namespace visora::api
