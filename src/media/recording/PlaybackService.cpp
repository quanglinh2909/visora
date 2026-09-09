#include "media/recording/PlaybackService.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

#include "core/Log.hpp"
#include "core/Time.hpp"

namespace visora::media {
namespace {
constexpr const char* kCategory = "playback";
}

PlaybackService::PlaybackService(PlaybackConfig config, std::shared_ptr<CameraService> cameras,
                                 std::shared_ptr<RecordingRepository> recordings,
                                 std::shared_ptr<ThumbnailExtractor> thumbnails)
    : m_config(std::move(config)),
      m_cameras(std::move(cameras)),
      m_recordings(std::move(recordings)),
      m_thumbnails(std::move(thumbnails)) {}

core::Result<std::vector<RecordingSegment>> PlaybackService::recordings(
    const std::string& cameraId, std::int64_t fromMs, std::int64_t toMs) {
    // Existence is checked here so an unknown camera is 404 rather than an
    // empty list, which a client cannot tell from "nothing recorded".
    auto camera = m_cameras->get(cameraId);
    if (!camera) return camera.error();
    if (toMs <= fromMs) return core::invalidArgument("the time range is empty");
    return m_recordings->segmentsInRange(cameraId, fromMs, toMs);
}

core::Result<std::string> PlaybackService::playlist(const std::string& cameraId,
                                                    std::int64_t fromMs, std::int64_t toMs) {
    auto segments = recordings(cameraId, fromMs, toMs);
    if (!segments) return segments.error();

    // Only finished segments. The one being written has no real end yet, and a
    // player told it is ten seconds long when it is two stalls at the join.
    std::vector<RecordingSegment> complete;
    for (const RecordingSegment& segment : segments.value()) {
        if (segment.status != SegmentStatus::Complete) continue;
        complete.push_back(segment);
    }
    return buildVodPlaylist(complete, m_config.playlist);
}

core::Result<SeekPoint> PlaybackService::seek(const std::string& cameraId, std::int64_t atMs) {
    auto camera = m_cameras->get(cameraId);
    if (!camera) return camera.error();

    // A day either side. Wide enough that a seek near a gap finds the next
    // recording, bounded so the query does not walk a year of segments.
    constexpr std::int64_t kWindowMs = 24LL * 60 * 60 * 1000;
    auto segments = m_recordings->segmentsInRange(cameraId, atMs - kWindowMs, atMs + kWindowMs);
    if (!segments) return segments.error();

    SeekPoint point = seekTo(segments.value(), atMs);
    if (!point.found) return core::notFound("nothing was recorded at or after that time");
    return point;
}

core::Result<PlayableFile> PlaybackService::resolveUnder(const std::string& baseDir,
                                                          const std::string& path,
                                                          const std::string& contentType) {
    std::error_code ec;
    const auto base = std::filesystem::weakly_canonical(std::filesystem::path(baseDir), ec);
    const auto target = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    if (ec) return core::notFound("the recording is no longer on disk");

    // lexically_relative starting with ".." means the target climbs out of the
    // base directory.
    const auto relative = target.lexically_relative(base);
    if (relative.empty() || relative.native().rfind("..", 0) == 0) {
        VS_WARN(kCategory) << "refusing to serve " << path << ": outside " << baseDir;
        return core::notFound("the recording is no longer on disk");
    }

    if (!std::filesystem::is_regular_file(target, ec)) {
        return core::notFound("the recording is no longer on disk");
    }
    const auto size = std::filesystem::file_size(target, ec);
    if (ec) return core::notFound("the recording is no longer on disk");

    PlayableFile file;
    file.path = target.string();
    file.size = static_cast<std::int64_t>(size);
    file.contentType = contentType;
    return file;
}

core::Result<PlayableFile> PlaybackService::segmentFile(const std::string& segmentId) {
    auto segment = m_recordings->segment(segmentId);
    if (!segment) return segment.error();
    return resolveUnder(m_config.recordingDir, segment.value().path, "video/mp2t");
}

core::Result<std::vector<MotionEvent>> PlaybackService::motionEvents(const std::string& cameraId,
                                                                     std::int64_t fromMs,
                                                                     std::int64_t toMs) {
    auto camera = m_cameras->get(cameraId);
    if (!camera) return camera.error();
    if (toMs <= fromMs) return core::invalidArgument("the time range is empty");
    return m_recordings->motionEventsInRange(cameraId, fromMs, toMs);
}

core::Result<PlayableFile> PlaybackService::motionEventImage(const std::string& eventId) {
    auto event = m_recordings->motionEvent(eventId);
    if (!event) return event.error();
    if (event.value().imagePath.empty()) {
        // The event is real; there was simply no picture branch running when it
        // happened. 404 on the image, not on the event.
        return core::notFound("this motion event has no image");
    }
    return resolveUnder(m_config.motionSnapshotDir, event.value().imagePath, "image/jpeg");
}

core::Result<std::vector<std::uint8_t>> PlaybackService::thumbnail(
    const std::string& cameraId, std::int64_t atMs, const ThumbnailOptions& options) {
    if (!m_thumbnails) return core::unsupported("this build cannot extract thumbnails");

    auto point = seek(cameraId, atMs);
    if (!point) return point.error();

    const RecordingSegment& segment = point.value().segment;
    auto file = resolveUnder(m_config.recordingDir, segment.path, "video/mp2t");
    if (!file) return file.error();

    return m_thumbnails->extract(file.value().path, segment.codec, point.value().offsetMs,
                                 options);
}

}  // namespace visora::media
