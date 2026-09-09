#pragma once

// Camera business rules.
//
// Everything the API does to a camera goes through here, and nothing here knows
// about HTTP or SQL. Dependencies arrive through the constructor rather than
// through a global component registry: a service locator hides what a class
// needs and makes a unit test drag in the whole application graph.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "media/camera/Camera.hpp"
#include "media/camera/CameraRepository.hpp"

namespace visora::media {

// What the service tells the world when a camera changes. The streaming layer
// subscribes; so does the websocket that pushes state to browsers.
//
// A callback rather than a direct call into the streaming layer, because the
// dependency has to point one way and the streaming layer already depends on
// the camera domain.
struct CameraEvents {
    std::function<void(const Camera&)> added;
    std::function<void(const Camera&, const CameraDiff&)> changed;
    std::function<void(const std::string& id)> removed;
};

class CameraService {
public:
    explicit CameraService(std::shared_ptr<CameraRepository> repository,
                           CameraEvents events = {});

    core::Result<std::vector<Camera>> list();
    core::Result<Camera> get(const std::string& id);

    // `name` and `rtsp` are required; everything else takes its default.
    core::Result<Camera> create(const CameraChanges& changes);

    core::Result<Camera> update(const std::string& id, const CameraChanges& changes);

    core::Status remove(const std::string& id);

    // Called by the streaming layer as a pipeline connects, fails or retries.
    core::Status reportRuntime(const std::string& id, CameraState state, Codec codec,
                               const std::string& outputRtsp, int retryCount,
                               const std::string& lastError);

private:
    std::shared_ptr<CameraRepository> m_repository;
    CameraEvents m_events;
};

// Validation, separated so it is testable on its own and so the same rules
// apply however a change arrives.
core::Status validateForCreate(const CameraChanges& changes);
core::Status validateForUpdate(const CameraChanges& changes);

// Whether a string is a plausible RTSP source. Deliberately permissive: a
// camera on an odd port with a path full of query parameters is normal, and
// rejecting a URL the camera would have accepted is worse than letting the
// connection attempt fail with a real error.
bool looksLikeRtspUrl(const std::string& url);

}  // namespace visora::media
