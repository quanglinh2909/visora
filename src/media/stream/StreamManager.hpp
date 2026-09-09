#pragma once

// Keeps every enabled camera streaming.
//
// One worker thread drives all of them: probe, publish, and retry with backoff
// when a camera is unreachable. A thread per camera would be simpler to write
// and would put a hundred mostly-sleeping threads on a board that has four
// cores to spare for video.
//
// It reacts to CameraService events rather than being called by it, so the
// dependency points one way: the camera domain does not know this exists.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media/camera/Camera.hpp"
#include "media/stream/RetryPolicy.hpp"
#include "media/stream/RtspServer.hpp"
#include "media/stream/StreamControl.hpp"

namespace visora::media {

struct StreamManagerConfig {
    std::string publicHost = "127.0.0.1";
    std::uint16_t rtspPort = 8554;
    int sourceLatencyMs = 300;
    RetryPolicy retry;
};

class StreamManager : public StreamControl {
public:
    StreamManager(StreamManagerConfig config, std::shared_ptr<RtspServer> server,
                  std::function<void(const std::string& cameraId, const StreamStatus&)> onStatus);
    ~StreamManager() override;

    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    core::Status start();
    void stop();

    // Brings a camera under management, or updates one already there. Safe to
    // call for a camera that has not changed: an unchanged source is not
    // republished, so an operator renaming a camera does not interrupt viewers.
    void apply(const Camera& camera);

    void remove(const std::string& cameraId);

    // --- StreamControl -------------------------------------------------------

    // Everything currently managed, for the REST status endpoint.
    std::map<std::string, StreamStatus> statuses() const override;
    std::optional<StreamStatus> statusOf(const std::string& cameraId) const override;

    void startStream(const std::string& cameraId) override;
    void stopStream(const std::string& cameraId) override;
    void restartStream(const std::string& cameraId) override;

private:
    struct Session {
        Camera camera;
        StreamStatus status;
        // Whether an operator wants this camera streaming. Separate from
        // whether it IS streaming: a camera stopped by hand must stay stopped
        // across the retry loop, and "offline because it was stopped" is a
        // different thing from "offline because the camera is unreachable".
        bool desired = true;
        // When to try again. Zero means "as soon as possible".
        std::chrono::steady_clock::time_point nextAttempt{};
        bool published = false;
        // What was published, so an unchanged camera is left alone.
        std::string publishedSource;
        std::string publishedHardware;
    };

    void workerLoop();
    // Returns whether anything changed, so the caller only reports when it did.
    bool advance(Session& session);
    void publishSession(Session& session);
    void report(const std::string& cameraId, const StreamStatus& status);
    // Drops the mount and clears what was published. Caller holds the lock.
    void teardown(const std::string& cameraId, Session& session);

    StreamManagerConfig m_config;
    std::shared_ptr<RtspServer> m_server;
    std::function<void(const std::string&, const StreamStatus&)> m_onStatus;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<std::string, Session> m_sessions;
    std::atomic<bool> m_running{false};
    std::thread m_worker;
};

}  // namespace visora::media
