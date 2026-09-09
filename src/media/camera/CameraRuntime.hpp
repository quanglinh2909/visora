#pragma once

// Everything about a camera that is true only while the program is running.
//
// The stored row says what an operator configured; this says what is actually
// happening — whether the pipeline is up, which codec was found, what the last
// error was, and what the camera can see right now.
//
// It exists so a controller has ONE collaborator instead of three. Joining a
// camera row to its live stream status is a business decision (which fields,
// what a missing session means, whether a stopped camera is 404 or 200), and
// business decisions do not belong in an HTTP adapter.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "media/camera/CameraService.hpp"
#include "media/stream/SnapshotGrabber.hpp"
#include "media/stream/StreamControl.hpp"

namespace visora::media {

// A camera row joined to its live stream. Flat on purpose: it is what an
// operator's dashboard shows in one row, and a client should not have to call
// two endpoints and correlate them.
struct CameraRuntimeStatus {
    std::string id;
    std::string name;
    CameraState state = CameraState::Offline;
    std::string inputRtsp;
    std::string outputRtsp;
    Codec codec = Codec::Unknown;
    std::string hardware;
    bool recordingEnabled = false;
    int retryCount = 0;
    std::string lastError;
    std::string lastChangedAt;

    // Whether the streaming layer is trying to keep this camera up. Intent, not
    // reality: `state` says what is actually happening. A camera an operator
    // stopped and one that is simply unreachable are both not online, and a UI
    // offers "start" for the first and "retrying..." for the second.
    //
    // False also before the streaming layer has seen the camera at all, which
    // is a moment long enough to observe only right after startup.
    bool streaming = false;
};

class CameraRuntime {
public:
    // The grabber may be null on a machine built without it; snapshot() then
    // reports Unsupported rather than crashing, which is the same answer the
    // rest of the system gives for hardware it does not have.
    CameraRuntime(std::shared_ptr<CameraService> cameras,
                  std::shared_ptr<StreamControl> streams,
                  std::shared_ptr<SnapshotGrabber> snapshots = nullptr,
                  SnapshotOptions snapshotOptions = {});

    core::Result<std::vector<CameraRuntimeStatus>> statuses();
    core::Result<CameraRuntimeStatus> statusOf(const std::string& cameraId);

    // Idempotent, and each returns the status as it stands after the call. That
    // status is a snapshot of an asynchronous machine: start() returns before
    // the camera is online, because probing an RTSP source takes seconds and an
    // HTTP request must not wait for it. Clients follow /ws/camera-state.
    core::Result<CameraRuntimeStatus> start(const std::string& cameraId);
    core::Result<CameraRuntimeStatus> stop(const std::string& cameraId);
    core::Result<CameraRuntimeStatus> restart(const std::string& cameraId);

    // A JPEG of what the camera sees now. Blocking — seconds, not milliseconds.
    core::Result<std::vector<std::uint8_t>> snapshot(const std::string& cameraId);

private:
    core::Result<CameraRuntimeStatus> statusFor(const Camera& camera);

    std::shared_ptr<CameraService> m_cameras;
    std::shared_ptr<StreamControl> m_streams;
    std::shared_ptr<SnapshotGrabber> m_snapshots;
    SnapshotOptions m_snapshotOptions;
};

}  // namespace visora::media
