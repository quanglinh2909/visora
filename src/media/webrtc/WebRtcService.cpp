#include "media/webrtc/WebRtcService.hpp"

#include <utility>

#include "core/Log.hpp"
#include "core/Time.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "webrtc";

// How often dead sessions are collected. Sessions cost a pipeline each, so this
// is not merely tidiness — a browser that crashes an hour into a shift must not
// leave one running until the process restarts.
constexpr auto kSweepInterval = std::chrono::seconds(5);

}  // namespace

WebRtcService::WebRtcService(WhepConfig config, std::shared_ptr<CameraService> cameras,
                             std::shared_ptr<CameraSourceRegistry> sources)
    : m_config(std::move(config)),
      m_cameras(std::move(cameras)),
      m_sources(std::move(sources)) {}

WebRtcService::~WebRtcService() { stop(); }

core::Status WebRtcService::start() {
    if (m_running.exchange(true)) return {};
    m_sweeper = std::thread([this] { sweepLoop(); });
    return {};
}

void WebRtcService::stop() {
    if (!m_running.exchange(false)) return;
    m_wake.notify_all();
    if (m_sweeper.joinable()) m_sweeper.join();

    std::map<std::string, std::shared_ptr<WhepSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sessions.swap(m_sessions);
    }
    // Outside the lock: tearing a webrtcbin down takes time, and holding the
    // service's lock through all of them would block every request.
    for (auto& [id, session] : sessions) session->stop();
}

core::Result<WhepAnswer> WebRtcService::offer(const WhepOffer& request) {
    auto camera = m_cameras->get(request.cameraId);
    if (!camera) return camera.error();
    if (request.sdp.empty()) return core::invalidArgument("the offer is empty");

    // The codec comes from the streaming layer's probe. Without it we cannot
    // say what the browser will receive, and guessing produces a session that
    // negotiates and then shows nothing.
    if (camera.value().codec == Codec::Unknown) {
        return core::unsupported("this camera is not streaming yet");
    }

    auto source = m_sources->acquire(request.cameraId, camera.value().inputRtsp,
                                     camera.value().codec);
    if (!source) return source.error();

    const std::string sessionId =
        request.cameraId + '-' + std::to_string(m_nextSessionId.fetch_add(1));

    auto session = std::make_shared<WhepSession>(sessionId, request.cameraId, source.value(),
                                                 m_config);
    auto answer = session->start(request.sdp, request.clientAddress);
    if (!answer) return answer.error();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions[sessionId] = session;
    }

    WhepAnswer out;
    out.sessionId = sessionId;
    out.sdp = answer.value();
    out.location = "/cameras/" + request.cameraId + "/whep/" + sessionId;
    return out;
}

core::Status WebRtcService::close(const std::string& sessionId) {
    std::shared_ptr<WhepSession> session;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) return core::notFound("no session " + sessionId);
        session = it->second;
        m_sessions.erase(it);
    }
    session->stop();
    VS_INFO(kCategory) << sessionId << ": closed by the client";
    return {};
}

std::vector<ViewerInfo> WebRtcService::viewers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ViewerInfo> out;
    out.reserve(m_sessions.size());
    for (const auto& [id, session] : m_sessions) out.push_back(session->info());
    return out;
}

void WebRtcService::sweepLoop() {
    while (m_running.load()) {
        std::vector<std::shared_ptr<WhepSession>> dead;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, kSweepInterval, [this] { return !m_running.load(); });
            if (!m_running.load()) break;

            for (auto it = m_sessions.begin(); it != m_sessions.end();) {
                if (it->second->alive()) {
                    ++it;
                    continue;
                }
                dead.push_back(it->second);
                it = m_sessions.erase(it);
            }
        }
        // Stopped outside the lock: see stop().
        for (auto& session : dead) {
            VS_INFO(kCategory) << session->id() << ": reaped (the viewer went away)";
            session->stop();
        }
    }
}

}  // namespace visora::media
