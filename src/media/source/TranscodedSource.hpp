#pragma once

// An EncodedSource that re-encodes another one to H.264.
//
// WHY THIS EXISTS: a good half of the cameras in a real installation send
// H.265, and several consumers cannot take it. A browser's WebCodecs decoder is
// configured with an `avc1.*` string and Annex-B H.264; handed H.265 it finds
// no AVC parameter sets, falls back to a guess, and refuses the first frame
// with "a key frame is required after configure()" — a failure that looks like
// a broken stream and is really a codec nobody translated.
//
// SHARED, like the source it wraps. Ten viewers of one H.265 camera transcode
// once between them: decoding and re-encoding 1080p is the most expensive thing
// this program does, and doing it per viewer is what makes a board fall over at
// four of them. The registry hands out one of these per camera and lets it go
// when the last consumer does.
//
// It is an EncodedSource and nothing more, so every existing consumer — MoQ
// today, the RTSP restream or recording tomorrow — takes it without knowing.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/Result.hpp"
#include "media/source/EncodedSource.hpp"
#include "media/source/SinkFanout.hpp"

namespace visora::media {

struct TranscodeOptions {
    // 0 lets the encoder decide. The wrapper fills this from the upstream
    // source's measured bitrate when it has one, because an encoder left to
    // guess picks from resolution and frame rate and lands several times over
    // what the camera actually sends.
    int bitrateKbps = 0;
    // -1 follows the input's own keyframes. A GOP imposed here would mean a
    // viewer joining waits for OUR keyframe rather than the camera's.
    int gopSize = -1;

    // The transcode caches its own GOP, because its consumers are H.264 viewers
    // that would otherwise wait for the NEXT re-encoded keyframe.
    GopCacheLimits gopCache;
};

class TranscodedSource final : public EncodedSource {
public:
    TranscodedSource(std::string cameraId, std::shared_ptr<EncodedSource> upstream,
                     TranscodeOptions options = {});
    ~TranscodedSource() override;

    TranscodedSource(const TranscodedSource&) = delete;
    TranscodedSource& operator=(const TranscodedSource&) = delete;

    core::Status start();
    void stop();

    std::uint64_t addSink(Sink sink, SinkOptions options = {}) override;

    // How many consumers this transcode has. Zero means nobody is watching it,
    // which is what decides when it is retired.
    std::size_t sinkCount() const;
    void removeSink(std::uint64_t id) override;
    bool alive() const override;
    Codec codec() const override { return Codec::H264; }
    std::uint64_t bitrateBps() const override;

    const std::string& cameraId() const { return m_cameraId; }

    // Exposed so the pipeline can be asserted without a camera.
    static std::string launchFor(Codec sourceCodec, const TranscodeOptions& options);

    // Public because the appsink and bus callbacks are free functions that
    // take it, exactly as the shared source does.
    struct Impl;

private:
    std::string m_cameraId;
    std::shared_ptr<EncodedSource> m_upstream;
    TranscodeOptions m_options;
    std::uint64_t m_sinkId = 0;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::media
