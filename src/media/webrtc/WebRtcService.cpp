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
                             std::shared_ptr<CameraSourceRegistry> sources,
                             std::shared_ptr<RecordingRepository> recordings)
    : m_config(std::move(config)),
      m_cameras(std::move(cameras)),
      m_sources(std::move(sources)),
      m_recordings(std::move(recordings)) {}

std::string WebRtcService::nextSessionId(const std::string& cameraId) {
    return cameraId + '-' + std::to_string(m_nextSessionId.fetch_add(1));
}

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

    std::map<std::string, Session> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sessions.swap(m_sessions);
    }
    // Outside the lock: tearing a webrtcbin down takes time, and holding the
    // service's lock through all of them would block every request.
    for (auto& [id, session] : sessions) {
        session.whep->stop();
        if (session.playback) session.playback->stop();
    }
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

    const std::string sessionId = nextSessionId(request.cameraId);

    auto session = std::make_shared<WhepSession>(sessionId, request.cameraId, source.value(),
                                                 m_config);
    auto answer = session->start(request.sdp, request.clientAddress);
    if (!answer) return answer.error();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions[sessionId] = Session{session, nullptr};
    }

    WhepAnswer out;
    out.sessionId = sessionId;
    out.sdp = answer.value();
    out.location = "/cameras/" + request.cameraId + "/whep/" + sessionId;
    return out;
}

core::Result<WhepAnswer> WebRtcService::offerPlayback(const PlaybackOffer& request) {
    if (!m_recordings) return core::unsupported("this build has no recording index");

    auto camera = m_cameras->get(request.cameraId);
    if (!camera) return camera.error();
    if (request.sdp.empty()) return core::invalidArgument("the offer is empty");

    // The codec of the RECORDING, which is what will be sent — not whatever the
    // camera happens to be producing now. They differ after a camera is
    // reconfigured, and answering with the live codec would describe a stream
    // the browser never receives.
    auto segments = m_recordings->segmentsInRange(request.cameraId, request.atMs - 60'000,
                                                  request.atMs + 24LL * 60 * 60 * 1000);
    if (!segments) return segments.error();
    Codec codec = Codec::Unknown;
    for (const RecordingSegment& segment : segments.value()) {
        if (segment.status != SegmentStatus::Complete) continue;
        codec = segment.codec;
        break;
    }
    if (codec == Codec::Unknown) {
        return core::notFound("nothing was recorded at or after that time");
    }

    auto source = std::make_shared<PlaybackSource>(request.cameraId, m_recordings, codec,
                                                   request.atMs);
    const core::Status started = source->start();
    if (!started.ok()) return started.error();

    const std::string sessionId = nextSessionId(request.cameraId);
    auto session = std::make_shared<WhepSession>(sessionId, request.cameraId, source, m_config);
    auto answer = session->start(request.sdp, request.clientAddress);
    if (!answer) {
        source->stop();
        return answer.error();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions[sessionId] = Session{session, source};
    }

    WhepAnswer out;
    out.sessionId = sessionId;
    out.sdp = answer.value();
    out.location = "/playback/" + sessionId;
    return out;
}

core::Status WebRtcService::control(const std::string& sessionId,
                                    const PlaybackCommand& command) {
    std::shared_ptr<PlaybackSource> playback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) return core::notFound("no session " + sessionId);
        playback = it->second.playback;
    }
    // A live session has nothing to seek. Saying so beats silently doing
    // nothing, which looks to a client like a control that does not work.
    if (!playback) return core::invalidArgument("this is a live session, not a playback one");

    switch (command.action) {
        case PlaybackCommand::Action::Seek:   playback->seek(command.atMs); break;
        case PlaybackCommand::Action::Pause:  playback->setPaused(true); break;
        case PlaybackCommand::Action::Resume: playback->setPaused(false); break;
        case PlaybackCommand::Action::Rate:   playback->setRate(command.rate); break;
    }
    return {};
}

core::Result<PlaybackState> WebRtcService::playbackState(const std::string& sessionId) const {
    std::shared_ptr<PlaybackSource> playback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) return core::notFound("no session " + sessionId);
        playback = it->second.playback;
    }
    if (!playback) return core::invalidArgument("this is a live session, not a playback one");
    return playback->state();
}

core::Status WebRtcService::close(const std::string& sessionId) {
    Session session;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) return core::notFound("no session " + sessionId);
        session = it->second;
        m_sessions.erase(it);
    }
    session.whep->stop();
    if (session.playback) session.playback->stop();
    VS_INFO(kCategory) << sessionId << ": closed by the client";
    return {};
}

std::vector<ViewerInfo> WebRtcService::viewers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ViewerInfo> out;
    out.reserve(m_sessions.size());
    for (const auto& [id, session] : m_sessions) out.push_back(session.whep->info());
    return out;
}

void WebRtcService::sweepLoop() {
    while (m_running.load()) {
        std::vector<Session> dead;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, kSweepInterval, [this] { return !m_running.load(); });
            if (!m_running.load()) break;

            for (auto it = m_sessions.begin(); it != m_sessions.end();) {
                // A playback session that reached the end of the recording is
                // NOT dead: the client can still seek somewhere else, which is
                // the whole point of holding the connection open.
                if (it->second.whep->alive()) {
                    ++it;
                    continue;
                }
                dead.push_back(it->second);
                it = m_sessions.erase(it);
            }
        }
        // Stopped outside the lock: see stop().
        for (auto& session : dead) {
            VS_INFO(kCategory) << session.whep->id() << ": reaped (the viewer went away)";
            session.whep->stop();
            if (session.playback) session.playback->stop();
        }
    }
}

}  // namespace visora::media
