#pragma once

// Reading recordings back: what exists, where to resume, what to show.
//
// The business rules of playback, with no HTTP and no SQL in sight. Joining a
// requested window to the segments that cover it, deciding what a seek into a
// gap means, refusing to serve a file outside the recordings directory — those
// are decisions, and decisions do not belong in a controller.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "media/camera/CameraService.hpp"
#include "media/recording/HlsPlaylist.hpp"
#include "media/recording/RecordingRepository.hpp"
#include "media/recording/ThumbnailExtractor.hpp"

namespace visora::media {

struct PlaybackConfig {
    std::string recordingDir = "recordings";
    std::string motionSnapshotDir = "motion-snapshots";
    PlaylistOptions playlist;
};

// A file this server is willing to serve, with what an HTTP layer needs to
// serve it. Resolved here rather than in the controller so the containment
// check happens once, in the place that owns the rule.
struct PlayableFile {
    std::string path;
    std::int64_t size = 0;
    std::string contentType = "video/mp2t";
};

class PlaybackService {
public:
    PlaybackService(PlaybackConfig config, std::shared_ptr<CameraService> cameras,
                    std::shared_ptr<RecordingRepository> recordings,
                    std::shared_ptr<ThumbnailExtractor> thumbnails = nullptr);

    core::Result<std::vector<RecordingSegment>> recordings(const std::string& cameraId,
                                                          std::int64_t fromMs,
                                                          std::int64_t toMs);

    // The HLS playlist for a window. An empty window yields a valid, empty
    // playlist rather than an error: a camera with nothing recorded yet is
    // normal, and a player needs something it can parse.
    core::Result<std::string> playlist(const std::string& cameraId, std::int64_t fromMs,
                                       std::int64_t toMs);

    // Where playback resumes for an instant. Only FINISHED segments are
    // considered: the one being written is a file the muxer still holds open,
    // and nothing can play it.
    core::Result<SeekPoint> seek(const std::string& cameraId, std::int64_t atMs);

    // The most recent finished recording, for a caller that wants "the latest"
    // rather than a particular moment.
    core::Result<SeekPoint> latest(const std::string& cameraId);

    // The bytes of one segment. NotFound both for an unknown id and for a row
    // whose file has gone — from the client's side those are the same thing,
    // and saying which would leak the layout of the disk.
    core::Result<PlayableFile> segmentFile(const std::string& segmentId);

    core::Result<std::vector<MotionEvent>> motionEvents(const std::string& cameraId,
                                                        std::int64_t fromMs, std::int64_t toMs);

    core::Result<PlayableFile> motionEventImage(const std::string& eventId);

    // A preview frame from what was recorded at `atMs`. Pass 0 or less for the
    // most recent recording, which is what a request with no timestamp means.
    core::Result<std::vector<std::uint8_t>> thumbnail(const std::string& cameraId,
                                                      std::int64_t atMs,
                                                      const ThumbnailOptions& options);

private:
    // The finished segments around an instant.
    core::Result<std::vector<RecordingSegment>> completeAround(const std::string& cameraId,
                                                               std::int64_t atMs);

    // Refuses a path that escapes the directory it should be under.
    //
    // The paths come from our own database, so this is not input validation —
    // it is the check that a bug or a hand-edited row cannot turn this endpoint
    // into "read any file on the server". Cheap, and the failure it prevents is
    // total.
    core::Result<PlayableFile> resolveUnder(const std::string& baseDir,
                                            const std::string& path,
                                            const std::string& contentType);

    PlaybackConfig m_config;
    std::shared_ptr<CameraService> m_cameras;
    std::shared_ptr<RecordingRepository> m_recordings;
    std::shared_ptr<ThumbnailExtractor> m_thumbnails;
};

}  // namespace visora::media
