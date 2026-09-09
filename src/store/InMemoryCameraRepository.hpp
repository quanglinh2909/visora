#pragma once

// An in-memory CameraRepository.
//
// Not a toy: it is what makes the service's rules testable in milliseconds with
// no database, no container and no fixture teardown. The predecessor could test
// none of its service logic because every path went through oatpp-postgresql.
//
// It is also a usable single-node mode for a board with no PostgreSQL, which is
// how a demo or a bring-up on new hardware starts.

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "media/camera/CameraRepository.hpp"

namespace visora::store {

class InMemoryCameraRepository final : public media::CameraRepository {
public:
    core::Result<std::vector<media::Camera>> list() override;
    core::Result<media::Camera> get(const std::string& id) override;
    core::Result<media::Camera> insert(const media::Camera& camera) override;
    core::Result<media::Camera> update(const media::Camera& camera) override;
    core::Status remove(const std::string& id) override;
    core::Status updateRuntime(const std::string& id,
                               const media::CameraRuntimeFields& fields) override;

private:
    mutable std::mutex m_mutex;
    // Ordered, so list() is stable across calls. A test that depends on
    // iteration order of an unordered container is a test that fails on a
    // different machine.
    std::map<std::string, media::Camera> m_cameras;
    std::uint64_t m_nextId = 1;
};

}  // namespace visora::store
