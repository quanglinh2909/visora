#include "media/moq/MoqService.hpp"

#include <utility>

#include "core/Log.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "moq";
constexpr auto kSweepInterval = std::chrono::seconds(5);

}  // namespace

MoqService::MoqService(MoqConfig config, std::shared_ptr<CameraService> cameras,
                       std::shared_ptr<CameraSourceRegistry> sources,
                       std::shared_ptr<RecordingRepository> recordings)
    : m_config(std::move(config)),
      m_cameras(std::move(cameras)),
      m_sources(std::move(sources)),
      m_recordings(std::move(recordings)) {}

MoqService::~MoqService() { stop(); }

core::Status MoqService::start() {
    if (m_config.socketPath.empty()) {
        // Not an error. A deployment without a MoQ server is the common one,
        // and the endpoints will say so per request rather than at startup.
        return {};
    }
    if (m_running.exchange(true)) return {};
    m_sweeper = std::thread([this] { sweepLoop(); });
    return {};
}

void MoqService::stop() {
    if (m_running.exchange(false)) {
        m_wake.notify_all();
        if (m_sweeper.joinable()) m_sweeper.join();
    }

    std::map<std::string, Session> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sessions.swap(m_sessions);
    }
    for (auto& [id, session] : sessions) {
        session.feed->stop();
        if (session.playback) session.playback->stop();
    }
}

core::Result<MoqFeedInfo> MoqService::open(const MoqFeedRequest& request) {
    if (m_config.socketPath.empty()) {
        return core::unsupported("no MoQ server is configured");
    }
    if (request.feedId.empty()) return core::invalidArgument("a feed needs an id");

    auto camera = m_cameras->get(request.cameraId);
    if (!camera) return camera.error();

    const std::string sessionId =
        request.feedId + '-' + std::to_string(m_nextSessionId.fetch_add(1));

    std::shared_ptr<EncodedSource> source;
    std::shared_ptr<PlaybackSource> playback;

    if (request.mode == "playback") {
        if (!m_recordings) return core::unsupported("this build has no recording index");

        // The codec of the RECORDING, not of the live camera: they differ after
        // a camera is reconfigured, and the header tells the MoQ server which
        // one it is about to receive.
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

        playback = std::make_shared<PlaybackSource>(request.cameraId, m_recordings, codec,
                                                    request.atMs);
        const core::Status started = playback->start();
        if (!started.ok()) return started.error();
        source = playback;
    } else {
        if (camera.value().codec == Codec::Unknown) {
            return core::unsupported("this camera is not streaming yet");
        }
        auto live = m_sources->acquire(request.cameraId, camera.value().inputRtsp,
                                       camera.value().codec);
        if (!live) return live.error();
        source = live.value();
    }

    auto feed = std::make_shared<MoqFeed>(sessionId, request.cameraId, request.feedId,
                                          m_config.socketPath, source);
    const core::Status started = feed->start();
    if (!started.ok()) {
        if (playback) playback->stop();
        return started.error();
    }

    Session session{feed, playback, request.mode};
    MoqFeedInfo info = describe(session);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions[sessionId] = std::move(session);
    }
    return info;
}

core::Status MoqService::close(const std::string& sessionId) {
    Session session;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) return core::notFound("no MoQ feed " + sessionId);
        session = it->second;
        m_sessions.erase(it);
    }
    session.feed->stop();
    if (session.playback) session.playback->stop();
    VS_INFO(kCategory) << sessionId << ": closed";
    return {};
}

MoqFeedInfo MoqService::describe(const Session& session) const {
    MoqFeedInfo info;
    info.sessionId = session.feed->id();
    info.feedId = session.feed->feedId();
    info.cameraId = session.feed->cameraId();
    info.mode = session.mode;
    info.framesSent = session.feed->framesSent();
    info.framesDropped = session.feed->framesDropped();
    return info;
}

std::vector<MoqFeedInfo> MoqService::feeds(const std::string& cameraId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<MoqFeedInfo> out;
    for (const auto& [id, session] : m_sessions) {
        if (!cameraId.empty() && session.feed->cameraId() != cameraId) continue;
        out.push_back(describe(session));
    }
    return out;
}

void MoqService::sweepLoop() {
    while (m_running.load()) {
        std::vector<Session> dead;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, kSweepInterval, [this] { return !m_running.load(); });
            if (!m_running.load()) break;

            for (auto it = m_sessions.begin(); it != m_sessions.end();) {
                if (it->second.feed->alive()) {
                    ++it;
                    continue;
                }
                dead.push_back(it->second);
                it = m_sessions.erase(it);
            }
        }
        for (auto& session : dead) {
            VS_INFO(kCategory) << session.feed->id() << ": reaped (the MoQ server went away)";
            session.feed->stop();
            if (session.playback) session.playback->stop();
        }
    }
}

}  // namespace visora::media
