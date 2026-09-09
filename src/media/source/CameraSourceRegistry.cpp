#include "media/source/CameraSourceRegistry.hpp"

#include <utility>

#include "core/Log.hpp"

namespace visora::media {
namespace {
constexpr const char* kCategory = "source";
}

CameraSourceRegistry::CameraSourceRegistry(RtspSourceOptions options)
    : m_options(std::move(options)) {}

core::Result<std::shared_ptr<EncodedSource>> CameraSourceRegistry::acquire(
    const std::string& cameraId, const std::string& rtspUrl, Codec codec) {
    if (rtspUrl.empty()) return core::invalidArgument("camera has no RTSP source");
    if (codec == Codec::Unknown) return core::invalidArgument("camera codec is not known yet");

    std::lock_guard<std::mutex> lock(m_mutex);

    const auto it = m_entries.find(cameraId);
    if (it != m_entries.end()) {
        if (auto existing = it->second.source.lock()) {
            const bool sameStream = it->second.rtspUrl == rtspUrl && it->second.codec == codec;
            // A source that has died is not reused: its pipeline is in an
            // error state and will never produce another buffer.
            if (sameStream && existing->alive()) {
                return std::static_pointer_cast<EncodedSource>(existing);
            }
            VS_INFO(kCategory) << cameraId << ": replacing the shared source ("
                               << (sameStream ? "source died" : "stream changed") << ')';
        }
    }

    auto source = std::make_shared<RtspEncodedSource>(cameraId, rtspUrl, codec, m_options);
    const core::Status started = source->start();
    if (!started.ok()) return started.error();

    m_entries[cameraId] = Entry{source, rtspUrl, codec};
    return std::static_pointer_cast<EncodedSource>(source);
}

std::size_t CameraSourceRegistry::liveCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t count = 0;
    for (const auto& [id, entry] : m_entries) {
        if (!entry.source.expired()) ++count;
    }
    return count;
}

}  // namespace visora::media
