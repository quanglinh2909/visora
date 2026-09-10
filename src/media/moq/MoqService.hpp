#pragma once

// The MoQ feeds, and their lifetimes.
//
// Feeds are created by the MoQ server, not by a browser: it asks for one when a
// viewer subscribes and closes it when the last one leaves. So sessions are
// reaped the same way WebRTC's are — a server that crashes leaves nothing else
// to notice it by.

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Result.hpp"
#include "media/camera/CameraService.hpp"
#include "media/moq/MoqFeed.hpp"
#include "media/recording/RecordingRepository.hpp"
#include "media/source/CameraSourceRegistry.hpp"
#include "media/source/PlaybackSource.hpp"
#include "media/source/TranscodedSource.hpp"

namespace visora::media {

struct MoqConfig {
    // Where the MoQ server listens. Empty disables the feature, which is the
    // default: a server that is not deployed should not make every request fail
    // with a connection error.
    std::string socketPath;
};

struct MoqFeedRequest {
    std::string feedId;
    std::string cameraId;
    // "live" or "playback".
    std::string mode = "live";
    std::int64_t atMs = 0;  // playback only
};

struct MoqFeedInfo {
    std::string sessionId;
    std::string feedId;
    std::string cameraId;
    std::string mode;
    std::uint64_t framesSent = 0;
    std::uint64_t framesDropped = 0;
};

class MoqService {
public:
    MoqService(MoqConfig config, std::shared_ptr<CameraService> cameras,
               std::shared_ptr<CameraSourceRegistry> sources,
               std::shared_ptr<RecordingRepository> recordings = nullptr);
    ~MoqService();

    MoqService(const MoqService&) = delete;
    MoqService& operator=(const MoqService&) = delete;

    core::Status start();
    void stop();

    core::Result<MoqFeedInfo> open(const MoqFeedRequest& request);
    core::Status close(const std::string& sessionId);

    // Everything currently feeding, optionally for one camera.
    std::vector<MoqFeedInfo> feeds(const std::string& cameraId = {}) const;

private:
    struct Session {
        std::shared_ptr<MoqFeed> feed;
        std::shared_ptr<PlaybackSource> playback;  // playback mode only
        // The H.265 -> H.264 pass, when this session needed one of its own.
        // A LIVE session's transcode is shared and owned by the registry; only
        // playback builds a private one, because two people scrubbing the same
        // day are at different points in it.
        std::shared_ptr<TranscodedSource> transcoded;
        std::string mode;
    };

    void sweepLoop();
    MoqFeedInfo describe(const Session& session) const;

    MoqConfig m_config;
    std::shared_ptr<CameraService> m_cameras;
    std::shared_ptr<CameraSourceRegistry> m_sources;
    std::shared_ptr<RecordingRepository> m_recordings;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<std::string, Session> m_sessions;
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_nextSessionId{1};
    std::thread m_sweeper;
};

}  // namespace visora::media
