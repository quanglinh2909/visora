#pragma once

// The PostgreSQL CameraRepository.
//
// The only file in the persistence layer that knows SQL, and one of the few
// that knows oatpp. Everything above it sees the port declared in
// media/camera/CameraRepository.hpp, which is why the service's rules are
// tested against an in-memory implementation instead of a live database.
//
// The schema is the predecessor's, unchanged: an existing deployment's data has
// to keep working through the cutover.

#include <memory>
#include <string>

#include "media/camera/CameraRepository.hpp"

namespace oatpp::orm {
class Executor;
}

namespace visora::store {

class PostgresCameraRepository final : public media::CameraRepository {
public:
    explicit PostgresCameraRepository(std::shared_ptr<oatpp::orm::Executor> executor);
    ~PostgresCameraRepository() override;

    core::Result<std::vector<media::Camera>> list() override;
    core::Result<media::Camera> get(const std::string& id) override;
    core::Result<media::Camera> insert(const media::Camera& camera) override;
    core::Result<media::Camera> update(const media::Camera& camera) override;
    core::Status remove(const std::string& id) override;
    core::Status updateRuntime(const std::string& id,
                               const media::CameraRuntimeFields& fields) override;

private:
    struct Impl;
    // PIMPL so oatpp's ORM headers stay out of everything that constructs this.
    std::unique_ptr<Impl> m_impl;
};

// Builds a connection pool for `url` and returns an executor for it.
core::Result<std::shared_ptr<oatpp::orm::Executor>> makePostgresExecutor(
    const std::string& url, int maxConnections, int idleSeconds);

}  // namespace visora::store
