#pragma once

// An in-memory RecordingRepository.
//
// The timeline rules — which segments a window needs, where a seek lands, what
// retention may delete — are the part of recording most worth testing and the
// part that needs no disk. This is what lets that happen without PostgreSQL.
//
// It is also the single-node mode: a board with no database still records and
// plays back within one run of the program.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "media/recording/RecordingRepository.hpp"

namespace visora::store {

class InMemoryRecordingRepository final : public media::RecordingRepository {
public:
    core::Result<media::RecordingSegment> upsertSegment(
        const media::RecordingSegment& segment) override;
    core::Result<std::vector<media::RecordingSegment>> segmentsInRange(
        const std::string& cameraId, std::int64_t fromMs, std::int64_t toMs) override;
    core::Result<media::RecordingSegment> segment(const std::string& id) override;
    core::Result<std::vector<media::RecordingSegment>> segmentsEndingBefore(
        const std::string& cameraId, std::int64_t beforeMs) override;
    core::Status removeSegments(const std::vector<std::string>& ids) override;

    core::Result<media::MotionEvent> insertMotionEvent(const media::MotionEvent& event) override;
    core::Status closeMotionEvent(const std::string& id, std::int64_t endMs, double maxScore,
                                  const std::string& cells) override;
    core::Status setMotionEventImage(const std::string& id,
                                     const std::string& imagePath) override;
    core::Result<std::vector<media::MotionEvent>> motionEventsInRange(
        const std::string& cameraId, std::int64_t fromMs, std::int64_t toMs) override;
    core::Result<media::MotionEvent> motionEvent(const std::string& id) override;

private:
    mutable std::mutex m_mutex;
    // Ordered, so a listing is stable across calls. A test that depends on the
    // iteration order of an unordered container fails on a different machine.
    std::map<std::string, media::RecordingSegment> m_segments;
    // path -> id, because that is the key the writer has: the muxer tells us a
    // file opened and later that it closed, and the two messages share only the
    // name.
    std::map<std::string, std::string> m_segmentByPath;
    std::map<std::string, media::MotionEvent> m_motionEvents;
    std::uint64_t m_nextSegmentId = 1;
    std::uint64_t m_nextEventId = 1;
};

}  // namespace visora::store
