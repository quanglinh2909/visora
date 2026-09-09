#include "media/ai/MotionEventRecorder.hpp"

#include <sstream>
#include <utility>

#include "core/Log.hpp"
#include "core/Time.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "motion";

void collect(const std::string& cells, std::set<std::string>& into) {
    std::size_t at = 0;
    while (at <= cells.size()) {
        const auto comma = cells.find(',', at);
        const std::string token =
            cells.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!token.empty()) into.insert(token);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
}

std::string join(const std::set<std::string>& cells) {
    std::string out;
    for (const std::string& cell : cells) {
        if (!out.empty()) out += ',';
        out += cell;
    }
    return out;
}

}  // namespace

MotionEventRecorder::MotionEventRecorder(std::shared_ptr<RecordingRepository> recordings,
                                         MotionEventRecorderConfig config)
    : m_recordings(std::move(recordings)) {
    if (config.snapshotDir.empty()) return;

    auto repository = m_recordings;
    m_snapshots = std::make_unique<MotionSnapshotWriter>(
        config.snapshotDir, config.jpegQuality,
        [repository](const std::string& eventId, const std::string& path) {
            const core::Status stored = repository->setMotionEventImage(eventId, path);
            if (!stored.ok()) {
                VS_WARN(kCategory) << "the event snapshot was written but not indexed: "
                                   << stored.error().message;
            }
        });
    const core::Status ready = m_snapshots->start();
    if (!ready.ok()) {
        // Events are still recorded; they simply have no picture. Refusing to
        // record them because a JPEG encoder is missing would lose the thing an
        // operator actually asked for.
        VS_WARN(kCategory) << "motion event snapshots are off: " << ready.error().message;
        m_snapshots.reset();
    }
}

MotionEventRecorder::~MotionEventRecorder() = default;

void MotionEventRecorder::setSaving(const std::string& cameraId, bool saving) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_saving[cameraId] = saving;
}

void MotionEventRecorder::observe(const MotionNotice& notice) {
    if (notice.cameraId.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto saving = m_saving.find(notice.cameraId);
    if (saving == m_saving.end() || !saving->second) return;

    const auto it = m_open.find(notice.cameraId);
    const bool isOpen = it != m_open.end();

    if (notice.triggered) {
        if (!isOpen) {
            MotionEvent event;
            event.cameraId = notice.cameraId;
            event.startMs = core::nowEpochMs();
            event.gridX = notice.gridX;
            event.gridY = notice.gridY;
            auto stored = m_recordings->insertMotionEvent(event);
            if (!stored) {
                VS_WARN(kCategory) << notice.cameraId << ": cannot record a motion event: "
                                   << stored.error().message;
                return;
            }
            // The picture, if this frame carried one. Written and indexed on
            // the writer's thread — the row exists now and gains its image
            // path a moment later, rather than naming a file that does not
            // exist yet.
            if (m_snapshots && notice.frame) {
                m_snapshots->capture(notice.cameraId, stored.value().id, notice.frame);
            }

            Open open;
            open.eventId = stored.value().id;
            open.startMs = event.startMs;
            collect(notice.insideCells, open.cells);
            m_open[notice.cameraId] = std::move(open);
            VS_DEBUG(kCategory) << notice.cameraId << ": motion event started";
            return;
        }
        // Cells accumulate over the WHOLE event, not just its last frame: what
        // an operator wants to see afterwards is everywhere something moved.
        collect(notice.insideCells, it->second.cells);
        return;
    }

    if (!isOpen) return;
    const core::Status closed = m_recordings->closeMotionEvent(
        it->second.eventId, core::nowEpochMs(), /*maxScore=*/1.0, join(it->second.cells));
    if (!closed.ok()) {
        VS_WARN(kCategory) << notice.cameraId << ": cannot close the motion event: "
                           << closed.error().message;
    }
    m_open.erase(it);
}

void MotionEventRecorder::closeAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [cameraId, open] : m_open) {
        m_recordings->closeMotionEvent(open.eventId, core::nowEpochMs(), 1.0,
                                       join(open.cells));
    }
    m_open.clear();
}

}  // namespace visora::media
