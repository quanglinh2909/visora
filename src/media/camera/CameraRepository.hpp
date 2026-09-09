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

// The fields the streaming layer owns. A struct rather than six positional
// parameters: they are written together, and a caller that swaps two strings of
// the same type gets a compile error here instead of a wrong row.
struct CameraRuntimeFields {
    CameraState state = CameraState::Offline;
    Codec codec = Codec::Unknown;
    std::string outputRtsp;
    int retryCount = 0;
    std::string lastError;
    // When this last changed, ISO-8601 UTC. Stamped by the service, not by the
    // adapter: the Postgres adapter used to invent it with now() and the
    // in-memory one left it empty, so the same operation was observably
    // different depending on where the data happened to live.
    std::string lastChangedAt;
};

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
    virtual core::Status updateRuntime(const std::string& id,
                                       const CameraRuntimeFields& fields) = 0;
};

}  // namespace visora::media
