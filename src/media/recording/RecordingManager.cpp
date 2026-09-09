#include "media/recording/RecordingManager.hpp"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <utility>
#include <vector>

#include "core/Log.hpp"
#include "core/Time.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "recording";
constexpr auto kMillisPerDay = 24LL * 60 * 60 * 1000;

}  // namespace

bool recordingNeedsRestart(const Camera& camera, const std::string& builtSource,
                           RecordingMode builtMode, int builtSegmentSeconds) {
    return camera.inputRtsp != builtSource || camera.recordingMode != builtMode ||
           camera.segmentSeconds != builtSegmentSeconds;
}

RecordingManager::RecordingManager(RecordingManagerConfig config,
                                   std::shared_ptr<RecordingRepository> repository,
                                   std::shared_ptr<CameraSourceRegistry> sources)
    : m_config(std::move(config)),
      m_repository(std::move(repository)),
      m_sources(std::move(sources)) {}

RecordingManager::~RecordingManager() { stop(); }

bool RecordingManager::shouldRecord(const Camera& camera) {
    if (camera.inputRtsp.empty()) return false;
    // Two switches say the same thing for historical reasons: `recordingEnabled`
    // predates `recordingMode`. Enabled with the mode left at Off means
    // continuous — the mode was simply never chosen.
    if (camera.recordingMode != RecordingMode::Off) return true;
    return camera.recordingEnabled;
}

core::Status RecordingManager::start() {
    if (m_running.exchange(true)) return {};
    m_worker = std::thread([this] { workerLoop(); });
    return {};
}

void RecordingManager::stop() {
    if (!m_running.exchange(false)) return;
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();

    // Flush every gate before the sessions go: a segment still held is a file
    // nothing will ever decide about, and a file nobody deletes is a leak that
    // only shows up as a full disk weeks later.
    std::map<std::string, Entry> entries;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        entries = std::move(m_entries);
        m_entries.clear();
    }
    for (auto& [id, entry] : entries) {
        if (entry.session) entry.session->stop();
        if (!entry.gate) continue;
        std::vector<RecordingSegment> unwanted;
        for (GateDecision& decision : entry.gate->flush()) {
            if (!decision.keep) unwanted.push_back(std::move(decision.segment));
        }
        deleteSegments(unwanted);
    }
}

void RecordingManager::apply(const Camera& camera, Codec codec) {
    // WHY THIS IS IN TWO LOCKED PHASES with a gap in the middle.
    //
    // Stopping a session cannot be done while holding m_mutex. stop() sends
    // EOS, waits for the muxer to finalise, and then JOINS the bus watcher —
    // and the last thing that watcher does is deliver "fragment closed", which
    // calls onSegment, which takes m_mutex. Holding it across stop() is a
    // deadlock by construction: the joiner waits for a thread that is waiting
    // for the joiner's own lock, and every later recording change waits behind
    // it for ever.
    //
    // It is not hypothetical and it is not rare: EOS is exactly what makes the
    // muxer close its last fragment, so the watcher is almost always in that
    // callback at that moment. remove() and stop() have always taken the
    // session out first and stopped it outside the lock; this did not, and
    // turning recording off on a camera that was recording hung the request.
    //
    // The gap is what m_applyMutex covers: without it a second apply() could
    // slip into the gap and build a rival session for the same camera. It
    // serialises configuration changes against each other, which costs a
    // configuration change on one camera a wait behind another camera's
    // finalise — seconds at worst, and only when both change at once.
    std::lock_guard<std::mutex> applying(m_applyMutex);

    std::unique_ptr<RecordingSession> retired;
    bool build = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Entry& entry = m_entries[camera.id];
        entry.camera = camera;
        entry.codec = codec;

        if (!shouldRecord(camera)) {
            if (entry.session) {
                VS_INFO(kCategory) << camera.id << ": recording turned off";
                retired = std::move(entry.session);
                entry.source.reset();
                entry.builtSource.clear();
            }
        } else if (codec == Codec::Unknown) {
            // The codec is discovered by the streaming layer's probe. Recording
            // waits for it rather than guessing: a pipeline built for the wrong
            // codec fails in a way that looks like a broken camera.
        } else if (entry.session && entry.session->running() &&
                   !recordingNeedsRestart(camera, entry.builtSource, entry.builtMode,
                                          entry.builtSegmentSeconds)) {
            // A rename reached us. Nothing to do — restarting here would
            // discard the segment currently being written.
        } else {
            retired = std::move(entry.session);
            build = true;
        }
    }

    if (retired) retired->stop();
    if (!build) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    Entry& entry = m_entries[camera.id];

    auto source = m_sources->acquire(camera.id, camera.inputRtsp, codec);
    if (!source) {
        VS_WARN(kCategory) << camera.id << ": cannot record: " << source.error().message;
        return;
    }
    entry.source = source.value();

    RecordingOptions options;
    options.recordingDir = m_config.recordingDir;
    options.segmentSeconds = std::max(1, camera.segmentSeconds);
    options.mode = camera.recordingMode == RecordingMode::Off ? RecordingMode::Continuous
                                                              : camera.recordingMode;

    MotionGateOptions gateOptions;
    gateOptions.preSeconds = camera.preMotionSeconds;
    gateOptions.postSeconds = camera.postMotionSeconds;
    entry.gate = std::make_unique<MotionGate>(gateOptions);

    const std::string cameraId = camera.id;
    entry.session = std::make_unique<RecordingSession>(
        cameraId, entry.source, options,
        [this, cameraId](const RecordingSegment& segment) { onSegment(cameraId, segment); });

    // Starting under the lock is safe where stopping is not: start() builds the
    // watcher rather than joining it, and onSegment takes m_mutex only for a
    // COMPLETE segment — which cannot arrive before the first one is cut.
    const core::Status started = entry.session->start();
    if (!started.ok()) {
        VS_WARN(kCategory) << camera.id << ": recording failed to start: "
                           << started.error().message;
        entry.session.reset();
        entry.source.reset();
        return;
    }

    entry.builtSource = camera.inputRtsp;
    entry.builtMode = options.mode;
    entry.builtSegmentSeconds = options.segmentSeconds;
    m_wake.notify_all();
}

void RecordingManager::remove(const std::string& cameraId) {
    Entry removed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.find(cameraId);
        if (it == m_entries.end()) return;
        removed = std::move(it->second);
        m_entries.erase(it);
    }
    // Outside the lock: stopping a session waits for the muxer to finalise, and
    // holding the manager's lock for seconds would stall every other camera.
    if (removed.session) removed.session->stop();
}

void RecordingManager::noteEvent(const std::string& cameraId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(cameraId);
    if (it == m_entries.end() || !it->second.gate) return;
    it->second.gate->noteEvent(core::nowEpochMs());
}

bool RecordingManager::isRecording(const std::string& cameraId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(cameraId);
    return it != m_entries.end() && it->second.session && it->second.session->running();
}

void RecordingManager::onSegment(const std::string& cameraId, const RecordingSegment& segment) {
    // Called on a GStreamer thread. Everything here must be quick and must not
    // throw: an exception escaping a GStreamer callback ends the process.
    RecordingSegment stored = segment;
    stored.cameraId = cameraId;

    auto saved = m_repository->upsertSegment(stored);
    if (!saved) {
        VS_WARN(kCategory) << cameraId << ": cannot record segment: " << saved.error().message;
        return;
    }

    // Only a CLOSED segment goes to the gate: an open one has no real end yet,
    // so no window can be decided against it.
    if (segment.status != SegmentStatus::Complete) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(cameraId);
    if (it == m_entries.end()) return;
    if (it->second.builtMode != RecordingMode::Motion) return;
    if (it->second.gate) it->second.gate->offer(saved.value());
}

std::vector<RecordingSegment> RecordingManager::settleGate(Entry& entry, std::int64_t nowMs) {
    // Caller holds the lock. Pure arithmetic, no I/O — see workerLoop.
    std::vector<RecordingSegment> unwanted;
    if (!entry.gate) return unwanted;
    for (GateDecision& decision : entry.gate->settle(nowMs)) {
        if (decision.keep) continue;
        unwanted.push_back(std::move(decision.segment));
    }
    if (!unwanted.empty()) {
        VS_DEBUG(kCategory) << entry.camera.id << ": dropping " << unwanted.size()
                            << " segment(s) with no event";
    }
    return unwanted;
}

std::vector<RecordingSegment> RecordingManager::expiredSegments(const std::string& cameraId,
                                                                int retentionDays,
                                                                std::int64_t nowMs) {
    // Called WITHOUT the lock: this queries the database.
    //
    // 0 days means keep everything — an installation that manages its own disk
    // should not have this deleting behind its back.
    if (retentionDays <= 0) return {};

    const std::int64_t cutoff = nowMs - static_cast<std::int64_t>(retentionDays) * kMillisPerDay;
    auto expired = m_repository->segmentsEndingBefore(cameraId, cutoff);
    if (!expired) {
        VS_WARN(kCategory) << cameraId << ": retention query failed: " << expired.error().message;
        return {};
    }
    if (!expired.value().empty()) {
        VS_INFO(kCategory) << cameraId << ": retention removing " << expired.value().size()
                           << " segment(s) older than " << retentionDays << " day(s)";
    }
    return expired.value();
}

void RecordingManager::deleteSegments(const std::vector<RecordingSegment>& segments) {
    if (segments.empty()) return;

    // Files first, then rows.
    //
    // The other order leaves rows pointing at files that are gone, which a
    // playlist happily lists and a player then fails to fetch. This order can
    // leave a row whose file is already gone only if the delete below fails,
    // and the next sweep retries it.
    std::vector<std::string> ids;
    ids.reserve(segments.size());
    for (const RecordingSegment& segment : segments) {
        std::error_code ec;
        std::filesystem::remove(segment.path, ec);
        if (ec) {
            VS_WARN(kCategory) << "cannot delete " << segment.path << ": " << ec.message();
        }
        ids.push_back(segment.id);
    }

    const core::Status removed = m_repository->removeSegments(ids);
    if (!removed.ok()) {
        VS_WARN(kCategory) << "cannot remove segment rows: " << removed.error().message;
    }
}

void RecordingManager::workerLoop() {
    VS_INFO(kCategory) << "recording manager started";
    const auto sweep = std::chrono::seconds(std::max(1, m_config.sweepSeconds));

    while (m_running.load()) {
        // Two phases, and the split is the point: everything under the lock is
        // arithmetic, everything that touches the database or the filesystem
        // happens after it is released. Deleting files while holding this lock
        // would block onSegment — which runs on a GStreamer thread — for as
        // long as the disk takes.
        std::vector<RecordingSegment> unwanted;
        std::vector<std::pair<std::string, int>> retention;
        std::int64_t nowMs = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, sweep, [this] { return !m_running.load(); });
            if (!m_running.load()) break;

            nowMs = core::nowEpochMs();
            for (auto& [id, entry] : m_entries) {
                std::vector<RecordingSegment> dropped = settleGate(entry, nowMs);
                unwanted.insert(unwanted.end(), std::make_move_iterator(dropped.begin()),
                                std::make_move_iterator(dropped.end()));
                if (entry.camera.retentionDays > 0) {
                    retention.emplace_back(entry.camera.id, entry.camera.retentionDays);
                }
            }
        }

        deleteSegments(unwanted);
        for (const auto& [cameraId, days] : retention) {
            deleteSegments(expiredSegments(cameraId, days, nowMs));
        }
    }
    VS_INFO(kCategory) << "recording manager stopped";
}

}  // namespace visora::media
