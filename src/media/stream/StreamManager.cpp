#include "media/stream/StreamManager.hpp"

#include <algorithm>
#include <chrono>

#include "core/Log.hpp"
#include "media/pipeline/CameraPipelines.hpp"
#include "media/stream/CodecProbe.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "stream";

// How often the worker wakes when nothing is due. Long enough to be idle,
// short enough that a retry scheduled a moment ago is not delayed much.
constexpr auto kIdlePoll = std::chrono::milliseconds(500);

bool wantsStreaming(const Camera& camera) {
    // Everything with a source is streamed: live viewing, recording and AI all
    // pull from the same restream, so there is no separate "enabled" flag to
    // honour here.
    return !camera.inputRtsp.empty();
}

}  // namespace

StreamManager::StreamManager(
    StreamManagerConfig config, std::shared_ptr<RtspServer> server,
    std::function<void(const std::string&, const StreamStatus&)> onStatus)
    : m_config(std::move(config)), m_server(std::move(server)), m_onStatus(std::move(onStatus)) {}

StreamManager::~StreamManager() { stop(); }

core::Status StreamManager::start() {
    if (m_running.exchange(true)) return {};
    m_worker = std::thread([this] { workerLoop(); });
    return {};
}

void StreamManager::stop() {
    if (!m_running.exchange(false)) return;
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, session] : m_sessions) {
        if (session.published) m_server->unpublish(mountPath(id));
    }
    m_sessions.clear();
}

void StreamManager::apply(const Camera& camera) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Session& session = m_sessions[camera.id];

        // Republish only when something the pipeline depends on moved. A rename
        // reaching here must not drop the viewers.
        const bool sourceMoved = session.publishedSource != camera.inputRtsp ||
                                 session.publishedHardware != camera.hardware;
        session.camera = camera;
        if (sourceMoved) {
            teardown(camera.id, session);
            session.status = StreamStatus{};
            // Pointing a camera at a new source re-arms it. Someone who edits
            // the URL of a stopped camera is fixing it, not asking for it to
            // stay dark until they also press start.
            session.desired = true;
            session.nextAttempt = std::chrono::steady_clock::time_point{};
        }
    }
    m_wake.notify_all();
}

void StreamManager::remove(const std::string& cameraId) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(cameraId);
        if (it == m_sessions.end()) return;
        if (it->second.published) m_server->unpublish(mountPath(cameraId));
        m_sessions.erase(it);
    }
    m_wake.notify_all();
}

std::map<std::string, StreamStatus> StreamManager::statuses() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, StreamStatus> out;
    for (const auto& [id, session] : m_sessions) {
        out[id] = session.status;
        out[id].desired = session.desired;
    }
    return out;
}

std::optional<StreamStatus> StreamManager::statusOf(const std::string& cameraId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sessions.find(cameraId);
    if (it == m_sessions.end()) return std::nullopt;
    StreamStatus status = it->second.status;
    status.desired = it->second.desired;
    return status;
}

void StreamManager::startStream(const std::string& cameraId) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(cameraId);
        if (it == m_sessions.end()) return;
        Session& session = it->second;
        if (session.desired) return;  // already wanted; nothing to do
        session.desired = true;
        // A camera stopped by hand and started again should try immediately,
        // not inherit the backoff it had accumulated before it was stopped.
        session.status = StreamStatus{};
        session.nextAttempt = std::chrono::steady_clock::time_point{};
    }
    m_wake.notify_all();
}

void StreamManager::stopStream(const std::string& cameraId) {
    StreamStatus stopped;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(cameraId);
        if (it == m_sessions.end()) return;
        Session& session = it->second;
        const bool wasDesired = session.desired;
        session.desired = false;
        teardown(cameraId, session);
        session.status = StreamStatus{};
        session.status.desired = false;
        // Far in the future: the worker must not quietly bring back a stream an
        // operator stopped. startStream() is what re-arms it.
        session.nextAttempt = std::chrono::steady_clock::time_point::max();
        if (!wasDesired) return;
        stopped = session.status;
    }
    m_wake.notify_all();
    report(cameraId, stopped);
}

void StreamManager::restartStream(const std::string& cameraId) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(cameraId);
        if (it == m_sessions.end()) return;
        Session& session = it->second;
        session.desired = true;
        teardown(cameraId, session);
        session.status = StreamStatus{};
        session.nextAttempt = std::chrono::steady_clock::time_point{};
    }
    m_wake.notify_all();
}

void StreamManager::teardown(const std::string& cameraId, Session& session) {
    if (session.published) m_server->unpublish(mountPath(cameraId));
    session.published = false;
    session.publishedSource.clear();
    session.publishedHardware.clear();
}

void StreamManager::workerLoop() {
    VS_INFO(kCategory) << "stream manager started";
    while (m_running.load()) {
        std::vector<std::pair<std::string, StreamStatus>> toReport;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, kIdlePoll, [this] { return !m_running.load(); });
            if (!m_running.load()) break;

            const auto now = std::chrono::steady_clock::now();
            for (auto& [id, session] : m_sessions) {
                if (session.nextAttempt > now) continue;
                // The probe blocks for seconds, so the lock is released around
                // it — otherwise one unreachable camera stalls every other
                // camera's status update and every REST call that reads them.
                Session copy = session;
                lock.unlock();
                const bool changed = advance(copy);
                lock.lock();

                const auto it = m_sessions.find(id);
                if (it == m_sessions.end()) continue;  // removed while we worked
                if (it->second.camera.inputRtsp != copy.camera.inputRtsp) continue;  // re-applied

                it->second.status = copy.status;
                it->second.nextAttempt = copy.nextAttempt;
                it->second.published = copy.published;
                it->second.publishedSource = copy.publishedSource;
                it->second.publishedHardware = copy.publishedHardware;
                if (changed) toReport.emplace_back(id, copy.status);
            }
        }
        for (const auto& [id, status] : toReport) report(id, status);
    }
    VS_INFO(kCategory) << "stream manager stopped";
}

bool StreamManager::advance(Session& session) {
    const Camera& camera = session.camera;

    if (!session.desired || !wantsStreaming(camera)) {
        if (session.status.state == CameraState::Offline && session.status.lastError.empty()) {
            return false;
        }
        session.status = StreamStatus{};
        return true;
    }
    if (session.published && session.status.state == CameraState::Online) {
        // Already streaming. The RTSP server keeps the pipeline alive and
        // restarts it per client; there is nothing to poll.
        session.nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        return false;
    }

    ProbeOptions options;
    options.latencyMs = m_config.sourceLatencyMs;
    auto codec = probeRtspCodec(camera.inputRtsp, options);

    if (!codec) {
        const bool permanent = codec.error().code == core::ErrorCode::Unsupported;
        const int attempt = session.status.retryCount;

        session.status.state = CameraState::Error;
        session.status.lastError = codec.error().message;
        session.status.codec = Codec::Unknown;
        session.status.outputRtsp.clear();

        if (permanent) {
            // A codec we cannot carry will not fix itself. Retry slowly rather
            // than never: someone reconfiguring the camera should not have to
            // restart the service.
            session.status.retryCount = attempt;
            session.nextAttempt = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(m_config.retry.maxMs);
        } else {
            session.status.retryCount = attempt + 1;
            session.nextAttempt =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(m_config.retry.delayFor(static_cast<std::uint32_t>(attempt)));
        }
        VS_WARN(kCategory) << camera.id << ": " << codec.error().message << " (attempt "
                           << session.status.retryCount << ')';
        return true;
    }

    session.status.codec = codec.value();
    publishSession(session);
    return true;
}

void StreamManager::publishSession(Session& session) {
    const Camera& camera = session.camera;

    CameraSource source;
    source.id = camera.id;
    source.rtspUrl = camera.inputRtsp;
    source.codec = session.status.codec;

    RestreamOptions options;
    options.latencyMs = m_config.sourceLatencyMs;

    const std::string launch = restreamLaunch(source, options);
    if (launch.empty()) {
        session.status.state = CameraState::Error;
        session.status.lastError = "no pipeline for codec " + std::string(toString(source.codec));
        return;
    }

    const core::Status published = m_server->publish(mountPath(camera.id), launch);
    if (!published.ok()) {
        session.status.state = CameraState::Error;
        session.status.lastError = published.error().message;
        session.nextAttempt = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(m_config.retry.maxMs);
        return;
    }

    session.published = true;
    session.publishedSource = camera.inputRtsp;
    session.publishedHardware = camera.hardware;
    session.status.state = CameraState::Online;
    session.status.retryCount = 0;
    session.status.lastError.clear();
    session.status.outputRtsp = "rtsp://" + m_config.publicHost + ':' +
                                std::to_string(m_config.rtspPort) + mountPath(camera.id);
    session.nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    VS_INFO(kCategory) << camera.id << " online as " << session.status.outputRtsp << " ("
                       << toString(session.status.codec) << ')';
}

void StreamManager::report(const std::string& cameraId, const StreamStatus& status) {
    if (m_onStatus) m_onStatus(cameraId, status);
}

}  // namespace visora::media
