#pragma once

// One RTSP connection to a camera, shared by every consumer of it.
//
// Owns a pipeline of the form
//     rtspsrc ! depay ! parse config-interval=-1 ! caps ! appsink
// and fans the access units out. Consumers pay only for what is genuinely
// different between them; the jitterbuffer, depayloader and parser happen once.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/Result.hpp"
#include "media/source/EncodedSource.hpp"

namespace visora::media {

struct RtspSourceOptions {
    int latencyMs = 300;

    // TCP by default, and it is not a preference.
    //
    // A 1080p or 4K keyframe arrives as one burst of hundreds of RTP packets.
    // Losing any of them corrupts the IDR for EVERY consumer — recording, AI
    // and every viewer at once — and shows up as a uniformly green picture. The
    // predecessor A/B-tested UDP on 16 cameras: identical CPU (160% either
    // way), nine reconnect warnings against one. There is nothing to win here.
    std::string protocols = "tcp";

    // Refuse the audio stream at SETUP rather than filtering it downstream.
    //
    // A capsfilter only discards audio AFTER rtspsrc has set it up, built a
    // jitterbuffer, an RTP session and two threads for it, and received and
    // reassembled every packet. Measured on the predecessor: 3 extra threads
    // per camera with audio, and RTP cost is per-packet. Set false only for a
    // camera whose SDP omits `media`, where declining would drop the video.
    bool videoOnly = true;
};

class RtspEncodedSource final : public EncodedSource,
                                public std::enable_shared_from_this<RtspEncodedSource> {
public:
    RtspEncodedSource(std::string cameraId, std::string rtspUrl, Codec codec,
                      RtspSourceOptions options = {});
    ~RtspEncodedSource() override;

    RtspEncodedSource(const RtspEncodedSource&) = delete;
    RtspEncodedSource& operator=(const RtspEncodedSource&) = delete;

    core::Status start();
    void stop();

    std::uint64_t addSink(Sink sink) override;
    void removeSink(std::uint64_t id) override;
    bool alive() const override;
    Codec codec() const override { return m_codec; }
    std::uint64_t bitrateBps() const override;

    const std::string& cameraId() const { return m_cameraId; }
    std::size_t sinkCount() const;

    // The launch description, separated so it is asserted without a camera.
    static std::string launchFor(const std::string& rtspUrl, Codec codec,
                                 const RtspSourceOptions& options);

    // The GStreamer state. Public only because the appsink and bus callbacks
    // are free functions that receive it as user data; nothing outside this
    // class's own translation unit names it.
    struct Impl;

private:
    std::string m_cameraId;
    std::string m_rtspUrl;
    Codec m_codec;
    RtspSourceOptions m_options;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::media
