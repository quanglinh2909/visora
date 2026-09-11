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

    // Each source reports its measured bitrate into the shared memory, so the
    // NEXT transcode of this camera knows what it sends even though nothing is
    // running yet when that transcode is built.
    RtspSourceOptions options = m_options;
    options.onBitrate = [memory = m_bitrates, cameraId](std::uint64_t bps) {
        if (bps > 0) memory->remember(cameraId, bps);
    };

    auto source = std::make_shared<RtspEncodedSource>(cameraId, rtspUrl, codec, options);
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

core::Result<std::shared_ptr<EncodedSource>> CameraSourceRegistry::acquireH264(
    const std::string& cameraId, const std::string& rtspUrl, Codec codec) {
    auto raw = acquire(cameraId, rtspUrl, codec);
    if (!raw) return raw;
    if (raw.value()->codec() == Codec::H264) return raw;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto held = m_transcodes[cameraId].lock()) {
        // Alive and reading the source this camera has now. A transcode left
        // over from a camera that has been repointed is transcoding the wrong
        // stream, so it is replaced rather than reused.
        if (held->alive()) return std::static_pointer_cast<EncodedSource>(held);
    }

    // The transcode inherits the deployment's GOP-cache setting: its consumers
    // are viewers, and turning the cache off for the camera while leaving it on
    // for the re-encode would be a setting that only half applies.
    TranscodeOptions transcodeOptions;
    transcodeOptions.gopCache = m_options.gopCache;
    // Live measurement first; it is the truth when it exists. Otherwise what
    // this camera measured last time, which is far closer to the truth than the
    // encoder's estimate from resolution alone.
    if (raw.value()->bitrateBps() == 0) {
        if (const std::uint64_t remembered = m_bitrates->recall(cameraId); remembered > 0) {
            transcodeOptions.bitrateKbps = static_cast<int>(remembered / 1000);
            VS_INFO(kCategory) << cameraId << ": encoding at " << transcodeOptions.bitrateKbps
                               << " kbps, remembered from this camera's last run";
        }
    }
    auto transcode = std::make_shared<TranscodedSource>(cameraId, raw.value(), transcodeOptions);
    const core::Status started = transcode->start();
    if (!started.ok()) return started.error();
    m_transcodes[cameraId] = transcode;
    return std::static_pointer_cast<EncodedSource>(transcode);
}

}  // namespace visora::media
