#include "media/recording/HlsPlaylist.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "core/Time.hpp"

namespace visora::media {
namespace {

// Below this, a difference between one segment's end and the next one's start
// is rounding, not a gap. Segment boundaries are decided by keyframe arrival,
// so adjacent files routinely differ by tens of milliseconds.
constexpr std::int64_t kGapToleranceMs = 2000;

bool needsDiscontinuity(const RecordingSegment& previous, const RecordingSegment& current) {
    // A new recording session means a new muxer and a new PTS base. This is the
    // case wall-clock comparison cannot detect.
    const bool sessionChanged = previous.sessionStartMs != current.sessionStartMs &&
                                (previous.sessionStartMs != 0 || current.sessionStartMs != 0);
    if (sessionChanged) return true;

    return current.startMs - previous.endMs > kGapToleranceMs;
}

}  // namespace

int targetDurationSeconds(const std::vector<RecordingSegment>& segments) {
    int target = 1;
    for (const RecordingSegment& segment : segments) {
        const int seconds =
            static_cast<int>(std::ceil(std::max(0, segment.durationMs) / 1000.0));
        target = std::max(target, seconds);
    }
    return target;
}

std::string buildVodPlaylist(const std::vector<RecordingSegment>& segments,
                             const PlaylistOptions& options) {
    std::ostringstream out;
    out << "#EXTM3U\n"
        << "#EXT-X-VERSION:3\n"
        << "#EXT-X-PLAYLIST-TYPE:VOD\n"
        << "#EXT-X-TARGETDURATION:" << targetDurationSeconds(segments) << "\n"
        << "#EXT-X-MEDIA-SEQUENCE:0\n";

    out.setf(std::ios::fixed);
    out.precision(3);

    for (std::size_t i = 0; i < segments.size(); ++i) {
        const RecordingSegment& segment = segments[i];
        if (i > 0 && needsDiscontinuity(segments[i - 1], segment)) {
            out << "#EXT-X-DISCONTINUITY\n";
        }
        // PROGRAM-DATE-TIME is what lets a client map a position in the
        // playlist back to a wall-clock instant, which is the whole point of a
        // timeline UI.
        out << "#EXT-X-PROGRAM-DATE-TIME:" << core::toIso8601Millis(segment.startMs) << "\n"
            << "#EXTINF:" << (std::max(0, segment.durationMs) / 1000.0) << ",\n"
            << options.segmentUrlPrefix << segment.id << options.segmentUrlSuffix << "\n";
    }

    out << "#EXT-X-ENDLIST\n";
    return out.str();
}

}  // namespace visora::media
