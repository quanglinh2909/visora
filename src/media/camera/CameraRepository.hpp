#pragma once

// The persistence PORT.
//
// Declared here, in the domain, and implemented in store/ — so the service
// depends on an interface it owns rather than on PostgreSQL. That inversion is
// what makes CameraService testable in milliseconds against an in-memory
// implementation, and what would let the storage engine change without the
// business logic noticing.

#include <optional>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "media/camera/Camera.hpp"

namespace visora::media {

class CameraRepository {
public:
    virtual ~CameraRepository() = default;

    virtual core::Result<std::vector<Camera>> list() = 0;

    // NotFound when the id does not exist — not an empty optional, so the
    // reason survives all the way to the HTTP status.
    virtual core::Result<Camera> get(const std::string& id) = 0;

    // Assigns the id and returns the stored row.
    virtual core::Result<Camera> insert(const Camera& camera) = 0;

    virtual core::Result<Camera> update(const Camera& camera) = 0;

    virtual core::Status remove(const std::string& id) = 0;

    // Runtime fields only. Separated from update() because they change on every
    // reconnect and must not collide with an operator editing the same row.
    virtual core::Status updateRuntime(const std::string& id, CameraState state,
                                       Codec codec, const std::string& outputRtsp,
                                       int retryCount, const std::string& lastError) = 0;
};

}  // namespace visora::media
