#include "store/InMemoryCameraRepository.hpp"

#include "store/Ids.hpp"

namespace visora::store {
namespace {

constexpr unsigned kCameraIdPrefix = 0;

std::string makeId(std::uint64_t counter) {
    return makeUuidLikeId(kCameraIdPrefix, counter);
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
                                                     const media::CameraRuntimeFields& fields) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_cameras.find(id);
    if (it == m_cameras.end()) return core::notFound("no camera with id " + id);
    it->second.state = fields.state;
    it->second.codec = fields.codec;
    it->second.outputRtsp = fields.outputRtsp;
    it->second.retryCount = fields.retryCount;
    it->second.lastError = fields.lastError;
    it->second.lastChangedAt = fields.lastChangedAt;
    return {};
}

}  // namespace visora::store
