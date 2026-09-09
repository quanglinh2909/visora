#pragma once

// What the API is allowed to do to a running stream.
//
// A port, not the manager itself. StreamManager owns a GStreamer RTSP server
// and a worker thread; a controller that took it directly could not be tested
// without both. Everything above this layer depends on the four verbs here, so
// a test substitutes a fake in one line and a second implementation (a remote
// node, say) needs no change above it.

#include <map>
#include <optional>
#include <string>

#include "media/camera/Camera.hpp"

namespace visora::media {

// How a camera's stream is doing, reported back so the row in the database and
// the websocket both reflect reality.
struct StreamStatus {
    CameraState state = CameraState::Offline;
    Codec codec = Codec::Unknown;
    std::string outputRtsp;
    int retryCount = 0;
    std::string lastError;

    // Whether the streaming layer is trying to keep this camera up.
    //
    // Distinct from `state`: intent versus reality. A camera an operator
    // stopped and one that is unreachable are both not streaming, and a UI
    // offers "start" for the first and "retrying..." for the second.
    bool desired = true;
};

class StreamControl {
public:
    virtual ~StreamControl() = default;

    virtual std::map<std::string, StreamStatus> statuses() const = 0;

    // Empty when the camera is not under management — which is not the same as
    // "offline", and the API says so with a 404.
    virtual std::optional<StreamStatus> statusOf(const std::string& cameraId) const = 0;

    // Idempotent. Starting a running stream, or stopping a stopped one, is a
    // no-op rather than an error: an operator clicking twice should not see a
    // failure, and a UI that lost track of state should be able to assert what
    // it wants.
    virtual void startStream(const std::string& cameraId) = 0;
    virtual void stopStream(const std::string& cameraId) = 0;

    // Tears the mount down and rebuilds it. The one operation that always does
    // something, because it is what an operator reaches for when the picture is
    // wrong and the state says everything is fine.
    virtual void restartStream(const std::string& cameraId) = 0;
};

}  // namespace visora::media
