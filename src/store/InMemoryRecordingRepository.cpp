#include "store/InMemoryRecordingRepository.hpp"

#include <algorithm>

#include "store/Ids.hpp"

namespace visora::store {
namespace {

constexpr unsigned kSegmentIdPrefix = 0x5e600001;  // "seg"
constexpr unsigned kEventIdPrefix = 0x30110001;    // "mot"

}  // namespace

core::Result<media::RecordingSegment> InMemoryRecordingRepository::upsertSegment(
    const media::RecordingSegment& segment) {
    if (segment.path.empty()) return core::invalidArgument("a segment needs a path");

    std::lock_guard<std::mutex> lock(m_mutex);

    const auto existing = m_segmentByPath.find(segment.path);
    if (existing != m_segmentByPath.end()) {
        const auto it = m_segments.find(existing->second);
        if (it != m_segments.end()) {
            media::RecordingSegment updated = segment;
            updated.id = it->second.id;
            // The opening message knows the motion state at the time it was
            // written; the closing one may have learned since. Never clear a
            // flag that was already set.
            updated.hasMotion = it->second.hasMotion || segment.hasMotion;
            if (updated.motionEventId.empty()) updated.motionEventId = it->second.motionEventId;
            it->second = updated;
            return it->second;
        }
    }

    media::RecordingSegment stored = segment;
    stored.id = makeUuidLikeId(kSegmentIdPrefix, m_nextSegmentId++);
    m_segments[stored.id] = stored;
    m_segmentByPath[stored.path] = stored.id;
    return stored;
}

core::Result<std::vector<media::RecordingSegment>>
InMemoryRecordingRepository::segmentsInRange(const std::string& cameraId, std::int64_t fromMs,
                                             std::int64_t toMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<media::RecordingSegment> all;
    for (const auto& [id, segment] : m_segments) {
        if (segment.cameraId != cameraId) continue;
        all.push_back(segment);
    }
    return media::segmentsForWindow(all, fromMs, toMs);
}

core::Result<media::RecordingSegment> InMemoryRecordingRepository::segment(
    const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_segments.find(id);
    if (it == m_segments.end()) return core::notFound("no recording segment with id " + id);
    return it->second;
}

core::Result<std::vector<media::RecordingSegment>>
InMemoryRecordingRepository::segmentsEndingBefore(const std::string& cameraId,
                                                  std::int64_t beforeMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<media::RecordingSegment> out;
    for (const auto& [id, segment] : m_segments) {
        if (segment.cameraId != cameraId) continue;
        // An open segment is being written to right now. Deleting the file
        // under the muxer is how a recording run ends in a broken pipeline.
        if (segment.status != media::SegmentStatus::Complete) continue;
        if (segment.endMs >= beforeMs) continue;
        out.push_back(segment);
    }
    std::sort(out.begin(), out.end(),
              [](const media::RecordingSegment& a, const media::RecordingSegment& b) {
                  return a.endMs < b.endMs;
              });
    return out;
}

core::Status InMemoryRecordingRepository::removeSegments(const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const std::string& id : ids) {
        const auto it = m_segments.find(id);
        if (it == m_segments.end()) continue;
        m_segmentByPath.erase(it->second.path);
        m_segments.erase(it);
    }
    return {};
}

core::Result<media::MotionEvent> InMemoryRecordingRepository::insertMotionEvent(
    const media::MotionEvent& event) {
    std::lock_guard<std::mutex> lock(m_mutex);
    media::MotionEvent stored = event;
    stored.id = makeUuidLikeId(kEventIdPrefix, m_nextEventId++);
    m_motionEvents[stored.id] = stored;
    return stored;
}

core::Status InMemoryRecordingRepository::closeMotionEvent(const std::string& id,
                                                           std::int64_t endMs, double maxScore,
                                                           const std::string& cells) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_motionEvents.find(id);
    if (it == m_motionEvents.end()) return core::notFound("no motion event with id " + id);
    it->second.endMs = endMs;
    it->second.maxScore = maxScore;
    it->second.cells = cells;
    return {};
}

core::Result<std::vector<media::MotionEvent>> InMemoryRecordingRepository::motionEventsInRange(
    const std::string& cameraId, std::int64_t fromMs, std::int64_t toMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<media::MotionEvent> out;
    for (const auto& [id, event] : m_motionEvents) {
        if (event.cameraId != cameraId) continue;
        // An open event has no end yet. Treat it as reaching to now, so a live
        // event shows up in the window it is happening in.
        const std::int64_t endMs = event.endMs == 0 ? toMs : event.endMs;
        if (!media::overlaps(event.startMs, endMs, fromMs, toMs)) continue;
        out.push_back(event);
    }
    std::sort(out.begin(), out.end(),
              [](const media::MotionEvent& a, const media::MotionEvent& b) {
                  return a.startMs < b.startMs;
              });
    return out;
}

core::Result<media::MotionEvent> InMemoryRecordingRepository::motionEvent(
    const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_motionEvents.find(id);
    if (it == m_motionEvents.end()) return core::notFound("no motion event with id " + id);
    return it->second;
}

}  // namespace visora::store
