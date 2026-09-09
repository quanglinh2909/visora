#include "media/camera/CameraService.hpp"

#include <algorithm>

#include "core/Log.hpp"

namespace visora::media {
namespace {
constexpr const char* kCategory = "camera";
constexpr std::size_t kMaxNameLength = 128;
constexpr std::size_t kMaxRtspLength = 512;
}  // namespace

bool looksLikeRtspUrl(const std::string& url) {
    if (url.size() < 8 || url.size() > kMaxRtspLength) return false;
    // rtsp:// and rtsps:// only. http sources are a different pipeline
    // entirely, and accepting one here produces a camera that never connects.
    return url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0;
}

core::Status validateForCreate(const CameraChanges& changes) {
    if (!changes.name.has_value() || changes.name->empty()) {
        return core::invalidArgument("name is required");
    }
    if (!changes.rtsp.has_value() || changes.rtsp->empty()) {
        return core::invalidArgument("rtsp is required");
    }
    return validateForUpdate(changes);
}

core::Status validateForUpdate(const CameraChanges& changes) {
    if (changes.name.has_value() && changes.name->size() > kMaxNameLength) {
        return core::invalidArgument("name exceeds " + std::to_string(kMaxNameLength) +
                                     " characters");
    }
    if (changes.rtsp.has_value() && !looksLikeRtspUrl(*changes.rtsp)) {
        return core::invalidArgument("rtsp must be an rtsp:// or rtsps:// URL");
    }
    if (changes.motionSensitivity.has_value() &&
        (*changes.motionSensitivity < 0.0 || *changes.motionSensitivity > 1.0)) {
        return core::invalidArgument("motionSensitivity must be between 0 and 1");
    }
    if (changes.motionThreshold.has_value() &&
        (*changes.motionThreshold < 0.0 || *changes.motionThreshold > 1.0)) {
        return core::invalidArgument("motionThreshold must be between 0 and 1");
    }
    // A segment length of zero makes splitmuxsink cut continuously; a very long
    // one makes a crash lose everything since the last split.
    if (changes.segmentSeconds.has_value() &&
        (*changes.segmentSeconds < 1 || *changes.segmentSeconds > 3600)) {
        return core::invalidArgument("segmentSeconds must be between 1 and 3600");
    }
    if (changes.preMotionSeconds.has_value() &&
        (*changes.preMotionSeconds < 0 || *changes.preMotionSeconds > 300)) {
        return core::invalidArgument("preMotionSeconds must be between 0 and 300");
    }
    if (changes.postMotionSeconds.has_value() &&
        (*changes.postMotionSeconds < 0 || *changes.postMotionSeconds > 300)) {
        return core::invalidArgument("postMotionSeconds must be between 0 and 300");
    }
    // 8..32 is what motioncells handles usefully: below 8 a cell covers so much
    // of the frame that a person walking through triggers everything, above 32
    // the per-cell cost stops being worth the resolution.
    if (changes.motionGridX.has_value() &&
        (*changes.motionGridX < 8 || *changes.motionGridX > 32)) {
        return core::invalidArgument("motionGridX must be between 8 and 32");
    }
    if (changes.motionGridY.has_value() &&
        (*changes.motionGridY < 8 || *changes.motionGridY > 32)) {
        return core::invalidArgument("motionGridY must be between 8 and 32");
    }
    if (changes.retentionDays.has_value() &&
        (*changes.retentionDays < 0 || *changes.retentionDays > 3650)) {
        return core::invalidArgument("retentionDays must be between 0 and 3650");
    }
    return {};
}

CameraService::CameraService(std::shared_ptr<CameraRepository> repository, CameraEvents events)
    : m_repository(std::move(repository)), m_events(std::move(events)) {}

core::Result<std::vector<Camera>> CameraService::list() { return m_repository->list(); }

core::Result<Camera> CameraService::get(const std::string& id) {
    if (id.empty()) return core::invalidArgument("camera id is required");
    return m_repository->get(id);
}

core::Result<Camera> CameraService::create(const CameraChanges& changes) {
    const core::Status valid = validateForCreate(changes);
    if (!valid.ok()) return valid.error();

    Camera camera;
    apply(changes, camera);
    camera.inputRtsp = camera.rtsp;

    auto stored = m_repository->insert(camera);
    if (!stored) return stored;

    VS_INFO(kCategory) << "created camera " << stored.value().id << " (" << stored.value().name
                       << ')';
    if (m_events.added) m_events.added(stored.value());
    return stored;
}

core::Result<Camera> CameraService::update(const std::string& id, const CameraChanges& changes) {
    if (id.empty()) return core::invalidArgument("camera id is required");

    const core::Status valid = validateForUpdate(changes);
    if (!valid.ok()) return valid.error();

    auto existing = m_repository->get(id);
    if (!existing) return existing;

    Camera camera = existing.value();
    const CameraDiff diff = apply(changes, camera);

    auto stored = m_repository->update(camera);
    if (!stored) return stored;

    // A rename should not interrupt a live stream. Only tell the streaming
    // layer when something it actually cares about moved.
    if (!diff.cosmeticOnly()) {
        VS_INFO(kCategory) << "camera " << id << " changed (source=" << diff.sourceChanged
                           << " recording=" << diff.recordingChanged
                           << " motion=" << diff.motionChanged << ')';
        if (m_events.changed) m_events.changed(stored.value(), diff);
    }
    return stored;
}

core::Status CameraService::remove(const std::string& id) {
    if (id.empty()) return core::invalidArgument("camera id is required");

    // Existence is checked first so deleting an unknown id is NotFound rather
    // than a silent success.
    auto existing = m_repository->get(id);
    if (!existing) return existing.error();

    const core::Status removed = m_repository->remove(id);
    if (!removed.ok()) return removed;

    VS_INFO(kCategory) << "removed camera " << id;
    // The pipeline is torn down after the row is gone, so a restart cannot
    // resurrect a camera the operator deleted.
    if (m_events.removed) m_events.removed(id);
    return {};
}

core::Status CameraService::reportRuntime(const std::string& id, CameraState state, Codec codec,
                                          const std::string& outputRtsp, int retryCount,
                                          const std::string& lastError) {
    return m_repository->updateRuntime(id, state, codec, outputRtsp, retryCount, lastError);
}

}  // namespace visora::media
