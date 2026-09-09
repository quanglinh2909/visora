#pragma once

// An in-memory AiJobRepository — what the job rules are tested against, and a
// usable single-node mode on a board with no PostgreSQL.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "vision/AiJobRepository.hpp"

namespace visora::store {

class InMemoryAiJobRepository final : public vision::AiJobRepository {
public:
    core::Result<std::vector<vision::AiJob>> list() override;
    core::Result<std::vector<vision::AiJob>> listForCamera(const std::string& cameraId) override;
    core::Result<vision::AiJob> get(const std::string& id) override;
    core::Result<vision::AiJob> insert(const vision::AiJob& job) override;
    core::Result<vision::AiJob> update(const vision::AiJob& job) override;
    core::Status remove(const std::string& id) override;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, vision::AiJob> m_jobs;
    std::uint64_t m_nextId = 1;
};

}  // namespace visora::store
