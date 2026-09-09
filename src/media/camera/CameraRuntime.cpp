#include "media/camera/CameraRuntime.hpp"

#include <utility>

namespace visora::media {

CameraRuntime::CameraRuntime(std::shared_ptr<CameraService> cameras,
                             std::shared_ptr<StreamControl> streams,
                             std::shared_ptr<SnapshotGrabber> snapshots,
                             SnapshotOptions snapshotOptions)
    : m_cameras(std::move(cameras)),
      m_streams(std::move(streams)),
      m_snapshots(std::move(snapshots)),
      m_snapshotOptions(snapshotOptions) {}

namespace {

CameraRuntimeStatus join(const Camera& camera, const StreamStatus* live) {
    CameraRuntimeStatus status;
    status.id = camera.id;
    status.name = camera.name;
    status.inputRtsp = camera.inputRtsp;
    status.hardware = camera.hardware;
    status.recordingEnabled = camera.recordingEnabled;
    status.lastChangedAt = camera.lastChangedAt;

    if (live) {
        // The live session wins over the row. The row is where the last known
        // state was persisted so a restart does not show every camera as
        // offline; once a session exists it is the truth.
        status.streaming = live->desired;
        status.state = live->state;
        status.codec = live->codec;
        status.outputRtsp = live->outputRtsp;
        status.retryCount = live->retryCount;
        status.lastError = live->lastError;
    } else {
        status.state = camera.state;
        status.codec = camera.codec;
        status.outputRtsp = camera.outputRtsp;
        status.retryCount = camera.retryCount;
        status.lastError = camera.lastError;
    }
    return status;
}

}  // namespace

core::Result<std::vector<CameraRuntimeStatus>> CameraRuntime::statuses() {
    auto cameras = m_cameras->list();
    if (!cameras) return cameras.error();

    // Read the whole map once. Asking the manager per camera would take its
    // lock once per row, and the set could change underneath the loop.
    const auto live = m_streams->statuses();

    std::vector<CameraRuntimeStatus> out;
    out.reserve(cameras.value().size());
    for (const Camera& camera : cameras.value()) {
        const auto it = live.find(camera.id);
        out.push_back(join(camera, it == live.end() ? nullptr : &it->second));
    }
    return out;
}

core::Result<CameraRuntimeStatus> CameraRuntime::statusFor(const Camera& camera) {
    const auto live = m_streams->statusOf(camera.id);
    return join(camera, live ? &*live : nullptr);
}

core::Result<CameraRuntimeStatus> CameraRuntime::statusOf(const std::string& cameraId) {
    auto camera = m_cameras->get(cameraId);
    if (!camera) return camera.error();
    return statusFor(camera.value());
}

core::Result<CameraRuntimeStatus> CameraRuntime::start(const std::string& cameraId) {
    auto camera = m_cameras->get(cameraId);
    if (!camera) return camera.error();
    if (camera.value().inputRtsp.empty()) {
        return core::invalidArgument("camera has no RTSP source to start");
    }
    m_streams->startStream(cameraId);
    return statusFor(camera.value());
}

core::Result<CameraRuntimeStatus> CameraRuntime::stop(const std::string& cameraId) {
    auto camera = m_cameras->get(cameraId);
    if (!camera) return camera.error();
    m_streams->stopStream(cameraId);
    return statusFor(camera.value());
}

core::Result<CameraRuntimeStatus> CameraRuntime::restart(const std::string& cameraId) {
    auto camera = m_cameras->get(cameraId);
    if (!camera) return camera.error();
    if (camera.value().inputRtsp.empty()) {
        return core::invalidArgument("camera has no RTSP source to restart");
    }
    m_streams->restartStream(cameraId);
    return statusFor(camera.value());
}

core::Result<std::vector<std::uint8_t>> CameraRuntime::snapshot(const std::string& cameraId) {
    auto camera = m_cameras->get(cameraId);
    if (!camera) return camera.error();
    if (!m_snapshots) return core::unsupported("this build cannot capture snapshots");
    if (camera.value().inputRtsp.empty()) {
        return core::invalidArgument("camera has no RTSP source");
    }

    // Straight from the camera, not from our own restream. Going through the
    // restream would make a snapshot depend on the stream being up, and the
    // most useful time to look at a camera is when something is wrong with it.
    return m_snapshots->grab(camera.value().inputRtsp, m_snapshotOptions);
}

}  // namespace visora::media
