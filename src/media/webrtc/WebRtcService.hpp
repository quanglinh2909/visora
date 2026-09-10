#pragma once

// The live WebRTC viewers, and their lifetimes.
//
// A viewer that closes its tab sends nothing. So sessions are reaped by a
// sweeper rather than trusted to announce themselves: a WHEP DELETE is the
// polite path and silence is the usual one.

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
#include "media/recording/RecordingRepository.hpp"
#include "media/source/CameraSourceRegistry.hpp"
#include "media/source/PlaybackSource.hpp"
#include "media/webrtc/WhepSession.hpp"

namespace visora::media {

struct WhepOffer {
    std::string cameraId;
    std::string sdp;
    // The browser's address as the HTTP layer sees it. Used only to replace an
    // mDNS name in an ICE candidate; empty is fine.
    std::string clientAddress;
};

struct WhepAnswer {
    std::string sessionId;
    std::string sdp;
    // Where the browser should send its DELETE. WHEP says the answer carries a
    // Location, and a client that follows the spec will use it.
    std::string location;
};

// Watching a RECORDING over the same transport as the live view.
//
// The alternative is HLS, and the difference is what happens when an operator
// clicks the timeline: HLS refetches a playlist describing the whole day and
// rebuilds the player, while this sends one seek command down a connection that
// is already open. See PlaybackSource.
struct PlaybackOffer {
    std::string cameraId;
    std::string sdp;
    std::string clientAddress;
    std::int64_t atMs = 0;
    // Speed to OPEN at, not just to change to later. A viewer scrubbing at 4x
    // whose connection drops reconnects mid-session, and starting that session
    // at 1x and waiting for a control message to correct it is a visible lurch.
    double rate = 1.0;
};

// What a client may do to a playback session in flight.
struct PlaybackCommand {
    enum class Action { Seek, Pause, Resume, Rate };
    Action action = Action::Seek;
    std::int64_t atMs = 0;
    double rate = 1.0;
};

class WebRtcService {
public:
    WebRtcService(WhepConfig config, std::shared_ptr<CameraService> cameras,
                  std::shared_ptr<CameraSourceRegistry> sources,
                  std::shared_ptr<RecordingRepository> recordings = nullptr);
    ~WebRtcService();

    WebRtcService(const WebRtcService&) = delete;
    WebRtcService& operator=(const WebRtcService&) = delete;

    core::Status start();
    void stop();

    // The camera must be streaming: its codec is discovered by probing, and
    // without it there is nothing to tell the browser it will receive.
    core::Result<WhepAnswer> offer(const WhepOffer& offer);

    // The camera must have something recorded at `atMs`.
    core::Result<WhepAnswer> offerPlayback(const PlaybackOffer& offer);

    core::Status control(const std::string& sessionId, const PlaybackCommand& command);
    core::Result<PlaybackState> playbackState(const std::string& sessionId) const;

    core::Status close(const std::string& sessionId);

    std::vector<ViewerInfo> viewers() const;

private:
    struct Session {
        std::shared_ptr<WhepSession> whep;
        // Present only for playback. Kept here because it is what the control
        // endpoint acts on, and because the session must outlive it.
        std::shared_ptr<PlaybackSource> playback;
    };

    void sweepLoop();
    std::string nextSessionId(const std::string& cameraId);

    WhepConfig m_config;
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
