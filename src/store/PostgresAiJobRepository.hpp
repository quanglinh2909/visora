#pragma once

// The PostgreSQL AiJobRepository.
//
// Column names and types are the predecessor's, unchanged, including the
// `stages` jsonb whose shape AiJobJson describes — so a deployment's jobs
// survive the cutover, and survive a rollback.

#include <memory>
#include <string>
#include <vector>

#include "vision/AiJobRepository.hpp"

namespace oatpp::orm {
class Executor;
}

namespace visora::store {

class PostgresAiJobRepository final : public vision::AiJobRepository {
public:
    explicit PostgresAiJobRepository(std::shared_ptr<oatpp::orm::Executor> executor);
    ~PostgresAiJobRepository();

    core::Result<std::vector<vision::AiJob>> list() override;
    core::Result<std::vector<vision::AiJob>> listForCamera(const std::string& cameraId) override;
    core::Result<vision::AiJob> get(const std::string& id) override;
    core::Result<vision::AiJob> insert(const vision::AiJob& job) override;
    core::Result<vision::AiJob> update(const vision::AiJob& job) override;
    core::Status remove(const std::string& id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::store
