#include "store/InMemoryAiJobRepository.hpp"

#include "store/Ids.hpp"

namespace visora::store {
namespace {
constexpr unsigned kJobIdPrefix = 0xA10B0001;  // "aijob"
}

core::Result<std::vector<vision::AiJob>> InMemoryAiJobRepository::list() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<vision::AiJob> out;
    out.reserve(m_jobs.size());
    for (const auto& [id, job] : m_jobs) out.push_back(job);
    return out;
}

core::Result<std::vector<vision::AiJob>> InMemoryAiJobRepository::listForCamera(
    const std::string& cameraId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<vision::AiJob> out;
    for (const auto& [id, job] : m_jobs) {
        if (job.cameraId == cameraId) out.push_back(job);
    }
    return out;
}

core::Result<vision::AiJob> InMemoryAiJobRepository::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_jobs.find(id);
    if (it == m_jobs.end()) return core::notFound("no AI job with id " + id);
    return it->second;
}

core::Result<vision::AiJob> InMemoryAiJobRepository::insert(const vision::AiJob& job) {
    std::lock_guard<std::mutex> lock(m_mutex);
    vision::AiJob stored = job;
    stored.id = makeUuidLikeId(kJobIdPrefix, m_nextId++);
    m_jobs[stored.id] = stored;
    return stored;
}

core::Result<vision::AiJob> InMemoryAiJobRepository::update(const vision::AiJob& job) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_jobs.find(job.id);
    if (it == m_jobs.end()) return core::notFound("no AI job with id " + job.id);
    it->second = job;
    return it->second;
}

core::Status InMemoryAiJobRepository::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_jobs.erase(id) == 0) return core::notFound("no AI job with id " + id);
    return {};
}

}  // namespace visora::store
