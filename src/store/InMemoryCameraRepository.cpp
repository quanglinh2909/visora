#include "store/InMemoryCameraRepository.hpp"

#include <cstdio>

namespace visora::store {
namespace {

// UUID-shaped so an id from this repository is interchangeable with one from
// PostgreSQL — including in a URL, where a differently shaped id would only
// break once someone switched backends.
std::string makeId(std::uint64_t counter) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "00000000-0000-4000-8000-%012llx",
                  static_cast<unsigned long long>(counter));
    return buffer;
}

}  // namespace

core::Result<std::vector<media::Camera>> InMemoryCameraRepository::list() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<media::Camera> out;
    out.reserve(m_cameras.size());
    for (const auto& [id, camera] : m_cameras) out.push_back(camera);
    return out;
}

core::Result<media::Camera> InMemoryCameraRepository::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_cameras.find(id);
    if (it == m_cameras.end()) return core::notFound("no camera with id " + id);
    return it->second;
}

core::Result<media::Camera> InMemoryCameraRepository::insert(const media::Camera& camera) {
    std::lock_guard<std::mutex> lock(m_mutex);
    media::Camera stored = camera;
    stored.id = makeId(m_nextId++);
    m_cameras[stored.id] = stored;
    return stored;
}

core::Result<media::Camera> InMemoryCameraRepository::update(const media::Camera& camera) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_cameras.find(camera.id);
    if (it == m_cameras.end()) return core::notFound("no camera with id " + camera.id);
    it->second = camera;
    return it->second;
}

core::Status InMemoryCameraRepository::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cameras.erase(id) == 0) return core::notFound("no camera with id " + id);
    return {};
}

core::Status InMemoryCameraRepository::updateRuntime(const std::string& id,
                                                     media::CameraState state, media::Codec codec,
                                                     const std::string& outputRtsp,
                                                     int retryCount,
                                                     const std::string& lastError) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_cameras.find(id);
    if (it == m_cameras.end()) return core::notFound("no camera with id " + id);
    it->second.state = state;
    it->second.codec = codec;
    it->second.outputRtsp = outputRtsp;
    it->second.retryCount = retryCount;
    it->second.lastError = lastError;
    return {};
}

}  // namespace visora::store
