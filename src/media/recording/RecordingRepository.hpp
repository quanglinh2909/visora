#pragma once

// The persistence PORT for recordings and motion events.
//
// Declared in the domain and implemented in store/, like CameraRepository —
// which is what lets the timeline, the playlist and the retention rules be
// tested against an in-memory implementation in milliseconds.

#include <cstdint>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "media/recording/Recording.hpp"

namespace visora::media {

class RecordingRepository {
public:
    virtual ~RecordingRepository() = default;

    // Inserts an open segment, or updates the one with the same path.
    //
    // Keyed on path rather than id because that is what the writer knows: the
    // muxer names the file, tells us when it opened it and again when it closed
    // it, and the two messages carry no shared id. An upsert also makes a
    // duplicate "opened" message — which a pipeline restart can produce —
    // harmless instead of a second row for one file.
    virtual core::Result<RecordingSegment> upsertSegment(const RecordingSegment& segment) = 0;

    // Segments overlapping [fromMs, toMs), in time order.
    virtual core::Result<std::vector<RecordingSegment>> segmentsInRange(
        const std::string& cameraId, std::int64_t fromMs, std::int64_t toMs) = 0;

    virtual core::Result<RecordingSegment> segment(const std::string& id) = 0;

    // Everything that ended before `beforeMs`, for retention. Returns the rows
    // rather than deleting them, because the files have to go too and a row
    // deleted before its file is a leak nobody will ever find.
    virtual core::Result<std::vector<RecordingSegment>> segmentsEndingBefore(
        const std::string& cameraId, std::int64_t beforeMs) = 0;

    virtual core::Status removeSegments(const std::vector<std::string>& ids) = 0;

    virtual core::Result<MotionEvent> insertMotionEvent(const MotionEvent& event) = 0;
    virtual core::Status closeMotionEvent(const std::string& id, std::int64_t endMs,
                                          double maxScore, const std::string& cells) = 0;
    virtual core::Result<std::vector<MotionEvent>> motionEventsInRange(
        const std::string& cameraId, std::int64_t fromMs, std::int64_t toMs) = 0;
    virtual core::Result<MotionEvent> motionEvent(const std::string& id) = 0;
};

}  // namespace visora::media
