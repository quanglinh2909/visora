#pragma once

// The HLS playlist for playing recordings back in a browser.
//
// Pure: segments in, playlist text out. No database, no filesystem, no
// GStreamer — which matters because the one rule in here that is hard to get
// right is also impossible to check by reading it, and easy to check by
// asserting the output.

#include <string>
#include <vector>

#include "media/recording/Recording.hpp"

namespace visora::media {

struct PlaylistOptions {
    // Where a segment's bytes are served from. A prefix rather than a full URL
    // so the playlist stays valid behind a reverse proxy on another host.
    std::string segmentUrlPrefix = "/recording-segments/";
    std::string segmentUrlSuffix = "/file";
};

// A VOD playlist: the whole recording, seekable, with an ENDLIST.
//
// Two things force an EXT-X-DISCONTINUITY between two adjacent segments, and
// both must be checked because neither implies the other:
//
//   1. They came from different recording SESSIONS. A new session is a new
//      mpegtsmux, which restarts its clock at ~3600 s — so the PTS jumps
//      backwards even though the wall clock did not. A player not told about
//      this stalls at the join or seeks to the wrong place. Wall-clock
//      comparison cannot see it: the gap between the two files can be
//      milliseconds.
//
//   2. There is a real gap in wall-clock time — the camera was down, or
//      recording was off. Tolerance is 2 s, so rounding between two adjacent
//      segments does not fabricate a discontinuity in a continuous recording.
std::string buildVodPlaylist(const std::vector<RecordingSegment>& segments,
                             const PlaylistOptions& options = {});

// Seconds, rounded up, of the longest segment. HLS requires this to be no
// smaller than any EXTINF, and a player rejects the whole playlist if it is.
int targetDurationSeconds(const std::vector<RecordingSegment>& segments);

}  // namespace visora::media
