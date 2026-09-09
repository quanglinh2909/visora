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
#include "media/source/CameraSourceRegistry.hpp"
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

class WebRtcService {
public:
    WebRtcService(WhepConfig config, std::shared_ptr<CameraService> cameras,
                  std::shared_ptr<CameraSourceRegistry> sources);
    ~WebRtcService();

    WebRtcService(const WebRtcService&) = delete;
    WebRtcService& operator=(const WebRtcService&) = delete;

    core::Status start();
    void stop();

    // The camera must be streaming: its codec is discovered by probing, and
    // without it there is nothing to tell the browser it will receive.
    core::Result<WhepAnswer> offer(const WhepOffer& offer);

    core::Status close(const std::string& sessionId);

    std::vector<ViewerInfo> viewers() const;

private:
    void sweepLoop();

    WhepConfig m_config;
    std::shared_ptr<CameraService> m_cameras;
    std::shared_ptr<CameraSourceRegistry> m_sources;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<std::string, std::shared_ptr<WhepSession>> m_sessions;
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_nextSessionId{1};
    std::thread m_sweeper;
};

}  // namespace visora::media
