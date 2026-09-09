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

MotionEventRecorder::MotionEventRecorder(std::shared_ptr<RecordingRepository> recordings)
    : m_recordings(std::move(recordings)) {}

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
