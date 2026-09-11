#include "media/source/CameraSourceRegistry.hpp"

#include <utility>
#include <vector>

#include "core/Log.hpp"

namespace visora::media {
namespace {
constexpr const char* kCategory = "source";

// How often the sweeper looks. Fine enough that a grace period of a few seconds
// is honoured to within a second, coarse enough to be free.
constexpr auto kSweepInterval = std::chrono::milliseconds(1000);
}  // namespace

CameraSourceRegistry::CameraSourceRegistry(RtspSourceOptions options)
    : m_options(std::move(options)) {
    m_sweeper = std::thread([this] { runSweeper(); });
}

CameraSourceRegistry::~CameraSourceRegistry() {
    m_stopping.store(true);
    m_wake.notify_all();
    if (m_sweeper.joinable()) m_sweeper.join();

    // Stop outside the lock, and stop transcodes before their upstreams: a
    // transcode still feeding from a source being torn down is a push into a
    // pipeline that is going away.
    std::map<std::string, Transcode> transcodes;
    std::map<std::string, Entry> entries;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        transcodes.swap(m_transcodes);
        entries.swap(m_entries);
    }
    for (auto& [id, transcode] : transcodes) {
        if (transcode.source) transcode.source->stop();
    }
    for (auto& [id, entry] : entries) {
        if (entry.source) entry.source->stop();
    }
}

void CameraSourceRegistry::runSweeper() {
    std::mutex waitMutex;
    while (!m_stopping.load()) {
        std::unique_lock<std::mutex> lock(waitMutex);
        m_wake.wait_for(lock, kSweepInterval, [this] { return m_stopping.load(); });
        if (m_stopping.load()) return;
        lock.unlock();
        sweep();
    }
}

void CameraSourceRegistry::sweepNow() { sweep(); }

void CameraSourceRegistry::sweep() {
    const auto now = IdleTimer::Clock::now();

    std::vector<std::shared_ptr<TranscodedSource>> retiringTranscodes;
    std::vector<std::pair<std::string, std::shared_ptr<RtspEncodedSource>>> retiringSources;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Transcodes first. One is a consumer of its upstream, so an upstream
        // can only be found idle on a later sweep, after its transcode has gone
        // — which is the right order anyway.
        for (auto it = m_transcodes.begin(); it != m_transcodes.end();) {
            auto& transcode = it->second;
            if (!transcode.source) {
                it = m_transcodes.erase(it);
                continue;
            }
            const bool dead = !transcode.source->alive();
            const bool unused = transcode.source->sinkCount() == 0;
            if (dead || transcode.idle.expired(!unused, now)) {
                if (!dead) {
                    VS_INFO(kCategory) << it->first << ": retiring the h264 transcode, unwatched";
                }
                retiringTranscodes.push_back(std::move(transcode.source));
                it = m_transcodes.erase(it);
                continue;
            }
            ++it;
        }

        for (auto it = m_entries.begin(); it != m_entries.end();) {
            auto& entry = it->second;
            if (!entry.source) {
                it = m_entries.erase(it);
                continue;
            }
            // A source whose pipeline has failed is retired at once whatever
            // the grace period says: it will never produce another buffer, and
            // holding it only delays the reconnect.
            const bool dead = !entry.source->alive();
            const bool unused = entry.source->sinkCount() == 0;
            if (dead || entry.idle.expired(!unused, now)) {
                if (!dead) {
                    VS_INFO(kCategory)
                        << it->first << ": retiring the camera source, unwatched for "
                        << entry.idle.idleFor(now).count() << " ms";
                }
                retiringSources.emplace_back(it->first, std::move(entry.source));
                it = m_entries.erase(it);
                continue;
            }
            ++it;
        }
    }

    // stop() joins the bus watcher, so it must not be called under the lock —
    // the watcher's own callbacks take it.
    for (auto& transcode : retiringTranscodes) transcode->stop();
    for (auto& [cameraId, source] : retiringSources) {
        (void)cameraId;
        source->stop();
    }
}

core::Result<std::shared_ptr<EncodedSource>> CameraSourceRegistry::acquire(
    const std::string& cameraId, const std::string& rtspUrl, Codec codec) {
    if (rtspUrl.empty()) return core::invalidArgument("camera has no RTSP source");
    if (codec == Codec::Unknown) return core::invalidArgument("camera codec is not known yet");

    std::lock_guard<std::mutex> lock(m_mutex);

    const auto it = m_entries.find(cameraId);
    if (it != m_entries.end() && it->second.source) {
        auto existing = it->second.source;
        const bool sameStream = it->second.rtspUrl == rtspUrl && it->second.codec == codec;
        // A source that has died is not reused: its pipeline is in an error
        // state and will never produce another buffer.
        if (sameStream && existing->alive()) {
            // Reviving one that was only being held by the grace period is the
            // point of the grace period: this viewer skips the whole RTSP
            // handshake because the stream never actually stopped.
            if (it->second.idle.idle()) {
                VS_INFO(kCategory) << cameraId << ": reusing the source that was still warm";
            }
            it->second.idle.expired(/*hasConsumers=*/true, IdleTimer::Clock::now());
            return std::static_pointer_cast<EncodedSource>(existing);
        }
        VS_INFO(kCategory) << cameraId << ": replacing the shared source ("
                           << (sameStream ? "source died" : "stream changed") << ')';
        // Dropped, not stopped: consumers still hold it and must be allowed to
        // finish. It dies when the last of them lets go, as it always did.
        m_entries.erase(it);
    }

    // Each source reports its measured bitrate into the shared memory, so the
    // NEXT transcode of this camera knows what it sends even though nothing is
    // running yet when that transcode is built.
    RtspSourceOptions options = m_options;
    options.onBitrate = [memory = m_bitrates, cameraId](std::uint64_t bps) {
        if (bps > 0) memory->remember(cameraId, bps);
    };

    auto source = std::make_shared<RtspEncodedSource>(cameraId, rtspUrl, codec, options);
    const core::Status started = source->start();
    if (!started.ok()) return started.error();

    Entry entry;
    entry.source = source;
    entry.rtspUrl = rtspUrl;
    entry.codec = codec;
    entry.idle = IdleTimer(m_options.idleLinger);
    m_entries[cameraId] = std::move(entry);
    return std::static_pointer_cast<EncodedSource>(source);
}

std::size_t CameraSourceRegistry::liveCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t count = 0;
    for (const auto& [id, entry] : m_entries) {
        (void)id;
        if (entry.source && entry.source->alive()) ++count;
    }
    return count;
}

std::size_t CameraSourceRegistry::lingeringCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t count = 0;
    for (const auto& [id, entry] : m_entries) {
        (void)id;
        if (entry.source && entry.idle.idle()) ++count;
    }
    return count;
}

core::Result<std::shared_ptr<EncodedSource>> CameraSourceRegistry::acquireH264(
    const std::string& cameraId, const std::string& rtspUrl, Codec codec, int bitrateKbps) {
    auto raw = acquire(cameraId, rtspUrl, codec);
    if (!raw) return raw;
    if (raw.value()->codec() == Codec::H264) return raw;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto held = m_transcodes.find(cameraId); held != m_transcodes.end() && held->second.source) {
        // Alive and reading the source this camera has now. A transcode left
        // over from a camera that has been repointed is transcoding the wrong
        // stream, so it is replaced rather than reused.
        if (held->second.source->alive() && held->second.bitrateKbps == bitrateKbps) {
            held->second.idle.expired(/*hasConsumers=*/true, IdleTimer::Clock::now());
            return std::static_pointer_cast<EncodedSource>(held->second.source);
        }
        // Dropped, not stopped: existing viewers keep the transcode they have
        // until they let go, and new ones get one built to the new setting.
        m_transcodes.erase(held);
    }

    // The transcode inherits the deployment's GOP-cache setting: its consumers
    // are viewers, and turning the cache off for the camera while leaving it on
    // for the re-encode would be a setting that only half applies.
    TranscodeOptions transcodeOptions;
    transcodeOptions.gopCache = m_options.gopCache;
    // An explicit per-camera setting wins over both: it exists precisely to say
    // "send this one smaller than it arrives", which following the source
    // cannot express.
    if (bitrateKbps > 0) {
        transcodeOptions.bitrateKbps = bitrateKbps;
        VS_INFO(kCategory) << cameraId << ": encoding at " << bitrateKbps
                           << " kbps, set on the camera";
    } else if (raw.value()->bitrateBps() == 0) {
        if (const std::uint64_t remembered = m_bitrates->recall(cameraId); remembered > 0) {
            transcodeOptions.bitrateKbps = static_cast<int>(remembered / 1000);
            VS_INFO(kCategory) << cameraId << ": encoding at " << transcodeOptions.bitrateKbps
                               << " kbps, remembered from this camera's last run";
        }
    }
    auto transcode = std::make_shared<TranscodedSource>(cameraId, raw.value(), transcodeOptions);
    const core::Status started = transcode->start();
    if (!started.ok()) return started.error();
    Transcode held;
    held.source = transcode;
    held.idle = IdleTimer(m_options.idleLinger);
    held.bitrateKbps = bitrateKbps;
    m_transcodes[cameraId] = std::move(held);
    return std::static_pointer_cast<EncodedSource>(transcode);
}

}  // namespace visora::media
