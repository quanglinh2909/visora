#pragma once

// What a recording is made of.
//
// Plain C++, like the camera entity: no oatpp, no SQL, no GStreamer. The
// timeline arithmetic — which segments a playback window needs, which ones a
// motion event should keep, which ones retention may delete — is the part most
// worth testing and the part that never needs hardware.
//
// Instants are milliseconds since the epoch, UTC. Formatting happens at the
// edges; comparing formatted strings is how an hour goes missing.

#include <cstdint>
#include <string>
#include <vector>

#include "media/camera/Camera.hpp"
#include "media/gst/Codec.hpp"

namespace visora::media {

// A segment is written before it is finished, so the live edge of a timeline
// exists rather than trailing by one segment length.
enum class SegmentStatus {
    // Open: the muxer is still writing this file. end/duration are an estimate.
    Recording,
    // Closed: end and duration are what the muxer actually produced.
    Complete,
};

const char* toString(SegmentStatus status);
SegmentStatus segmentStatusFromString(std::string_view text);

struct RecordingSegment {
    std::string id;
    std::string cameraId;
    std::string path;         // relative to the recording directory's parent
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    int durationMs = 0;
    Codec codec = Codec::Unknown;
    std::string container = "ts";
    RecordingMode recordingMode = RecordingMode::Continuous;
    bool hasMotion = false;
    std::string motionEventId;
    SegmentStatus status = SegmentStatus::Complete;

    // When the RECORDING SESSION that produced this segment started, in epoch
    // milliseconds. Every segment of one session carries the same value.
    //
    // Load-bearing for playback: a new session is a new pipeline, and a new
    // mpegtsmux restarts its clock at ~3600 s. Two segments that are adjacent
    // in wall-clock time but belong to different sessions have a PTS jump
    // between them, and a player that is not told so goes wrong at the join.
    // Wall-clock alone cannot detect this — the gap can be milliseconds.
    std::int64_t sessionStartMs = 0;
};

struct MotionEvent {
    std::string id;
    std::string cameraId;
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;      // 0 while the event is still open
    double maxScore = 0.0;
    // Cells that moved during the event, "row:col" comma-separated — the format
    // motioncells itself produces, kept so a UI can draw them back.
    std::string cells;
    int gridX = 0;
    int gridY = 0;
    // A frame from when the event started. Empty is valid: the event happened,
    // there just was no picture branch running.
    std::string imagePath;
};

// The window a motion event protects from deletion: the event itself plus the
// operator's pre- and post-roll.
struct MotionWindow {
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::int64_t preMs = 10'000;
    std::int64_t postMs = 20'000;

    std::int64_t keepFromMs() const { return startMs - (preMs < 0 ? 0 : preMs); }
    std::int64_t keepToMs() const { return endMs + (postMs < 0 ? 0 : postMs); }
};

// Half-open on the right: a segment ending exactly when a window starts does
// not overlap it. Getting this wrong keeps or drops one extra segment at every
// boundary, which over a day of ten-second segments is thousands of files.
bool overlaps(std::int64_t aStart, std::int64_t aEnd, std::int64_t bStart, std::int64_t bEnd);

bool segmentIsWorthKeeping(const RecordingSegment& segment, const MotionWindow& window);

// The segments a playback window needs, in time order, with anything outside it
// dropped. A segment that merely touches the window is included: playback has
// to start at a keyframe before the requested instant.
std::vector<RecordingSegment> segmentsForWindow(const std::vector<RecordingSegment>& segments,
                                                std::int64_t fromMs, std::int64_t toMs);

// Where playback should resume for an instant: the segment containing it and
// how far into that segment to seek. `offsetMs` is zero when `atMs` falls in a
// gap and the next segment is used instead — resuming mid-file at a point the
// file does not cover would seek past its end.
struct SeekPoint {
    bool found = false;
    RecordingSegment segment;
    std::int64_t offsetMs = 0;
};

SeekPoint seekTo(const std::vector<RecordingSegment>& segments, std::int64_t atMs);

// The file path for a new segment: "<dir>/<cameraId>/<local timestamp>.ts".
//
// Local time in the name and UTC in the database on purpose — see core/Time.
std::string segmentPath(const std::string& recordingDir, const std::string& cameraId,
                        const std::string& localTimestamp);

}  // namespace visora::media
