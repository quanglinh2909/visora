#include "media/recording/Recording.hpp"

#include <algorithm>

namespace visora::media {

const char* toString(SegmentStatus status) {
    switch (status) {
        case SegmentStatus::Recording: return "recording";
        case SegmentStatus::Complete:  return "complete";
    }
    return "complete";
}

SegmentStatus segmentStatusFromString(std::string_view text) {
    if (text == "recording") return SegmentStatus::Recording;
    return SegmentStatus::Complete;
}

bool overlaps(std::int64_t aStart, std::int64_t aEnd, std::int64_t bStart, std::int64_t bEnd) {
    return aStart < bEnd && bStart < aEnd;
}

bool segmentIsWorthKeeping(const RecordingSegment& segment, const MotionWindow& window) {
    return overlaps(segment.startMs, segment.endMs, window.keepFromMs(), window.keepToMs());
}

std::vector<RecordingSegment> segmentsForWindow(const std::vector<RecordingSegment>& segments,
                                                std::int64_t fromMs, std::int64_t toMs) {
    std::vector<RecordingSegment> out;
    for (const RecordingSegment& segment : segments) {
        if (!overlaps(segment.startMs, segment.endMs, fromMs, toMs)) continue;
        out.push_back(segment);
    }
    std::sort(out.begin(), out.end(),
              [](const RecordingSegment& a, const RecordingSegment& b) {
                  if (a.startMs != b.startMs) return a.startMs < b.startMs;
                  return a.id < b.id;  // stable for segments sharing a start
              });
    return out;
}

SeekPoint seekTo(const std::vector<RecordingSegment>& segments, std::int64_t atMs) {
    std::vector<RecordingSegment> ordered = segments;
    std::sort(ordered.begin(), ordered.end(),
              [](const RecordingSegment& a, const RecordingSegment& b) {
                  return a.startMs < b.startMs;
              });

    SeekPoint point;
    for (const RecordingSegment& segment : ordered) {
        if (atMs >= segment.startMs && atMs < segment.endMs) {
            point.found = true;
            point.segment = segment;
            point.offsetMs = atMs - segment.startMs;
            return point;
        }
        if (segment.startMs >= atMs) {
            // The instant falls in a gap. Start of the next segment, offset
            // zero: seeking to `atMs - startMs` here would be negative, and
            // clamping it to zero silently would hide the gap from the caller.
            point.found = true;
            point.segment = segment;
            point.offsetMs = 0;
            return point;
        }
    }
    return point;
}

std::string segmentPath(const std::string& recordingDir, const std::string& cameraId,
                        const std::string& localTimestamp) {
    return recordingDir + "/" + cameraId + "/" + localTimestamp + ".ts";
}

}  // namespace visora::media
