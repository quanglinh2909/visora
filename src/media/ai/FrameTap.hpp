#pragma once

// Decoded frames from a camera, for whoever needs pixels rather than bytes.
//
// Recording and WebRTC take a camera's ENCODED access units and never decode
// them, which is why adding either costs almost nothing. AI is the one consumer
// that needs actual pixels, so it is the one that pays for a decoder — and
// exactly one decoder per camera, however many jobs and detectors run on it.
//
// Frames are handed over as they come and are NOT queued. A consumer that is
// slower than the camera skips frames rather than falling behind, because
// analysing a frame from four seconds ago is worth nothing and the memory to
// hold it is not free. That is the right trade for inference and the wrong one
// for recording, which is why they are different paths.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/Image.hpp"
#include "core/Result.hpp"
#include "media/gst/BusWatcher.hpp"
#include "media/source/EncodedSource.hpp"

namespace visora::media {

struct FrameTapOptions {
    // Analysing every frame of every camera is rarely what anyone wants: a
    // detector at 5 fps sees everything that matters and leaves the accelerator
    // for the other cameras. 0 means every frame.
    int maxFps = 5;

    // Decoded frames are handed over in this format. NV12 because that is what
    // hardware decoders produce and what the image layer converts from most
    // cheaply — asking for RGB here would insert a CPU conversion per frame per
    // camera, which measurably is the most expensive thing in the pipeline.
    core::PixelFormat format = core::PixelFormat::NV12;
};

class FrameTap {
public:
    // The frame is BORROWED and valid only for the duration of the call. A
    // consumer that wants to keep it copies it.
    using Sink = std::function<void(const core::ImageView&, std::int64_t tsUs)>;

    FrameTap(std::string cameraId, std::shared_ptr<EncodedSource> source,
             FrameTapOptions options = {});
    ~FrameTap();

    FrameTap(const FrameTap&) = delete;
    FrameTap& operator=(const FrameTap&) = delete;

    core::Status start();
    void stop();

    std::uint64_t addSink(Sink sink);
    void removeSink(std::uint64_t id);
    std::size_t sinkCount() const;

    bool running() const;

    // The launch description, separated so it is asserted without a camera.
    static std::string launchFor(Codec codec, const FrameTapOptions& options);

    // The GStreamer state. Public only because the appsink and bus callbacks
    // are free functions that receive it; nothing outside this class's own
    // translation unit names it.
    struct Impl;

private:
    std::string m_cameraId;
    std::shared_ptr<EncodedSource> m_source;
    FrameTapOptions m_options;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::media
