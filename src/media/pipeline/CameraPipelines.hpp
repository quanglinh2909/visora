#pragma once

// The pipelines a camera needs, built from CodecProvider elements.
//
// These replace the launch strings the predecessor assembled by hand in ten
// places. Everything here is a pure function of its inputs, so it is tested
// with golden strings on a machine with no camera — which is the whole reason
// for building pipelines out of ElementSpec rather than out of ostringstream.

#include <string>

#include "media/gst/Codec.hpp"
#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {

struct CameraSource {
    std::string id;
    std::string rtspUrl;
    Codec codec = Codec::Unknown;
};

struct RestreamOptions {
    // Latency the jitter buffer is allowed.
    //
    // Floored at 300 ms deliberately, and drop-on-latency is left OFF. A camera
    // sends large IDR frames — 4K is hundreds of TCP packets back to back — and
    // on a busy board the whole burst misses the deadline. The jitter buffer
    // then discards its tail, the IDR arrives incomplete, and EVERY viewer
    // (RTSP and WebRTC alike) gets a stream that will not decode: black or
    // frozen in bursts that track machine load. This branch only serves live
    // viewing, where a few hundred extra milliseconds cost nothing. Real-time
    // deadlines belong to the AI branch, which has its own source.
    int latencyMs = 300;
    static constexpr int kMinimumLatencyMs = 300;
};

// Restream a camera as-is: no decode, no re-encode, just repackage RTP.
//
// Wrapped in parentheses because it feeds gst_rtsp_media_factory_set_launch.
// Returns empty for a codec with no payloader.
std::string restreamLaunch(const CameraSource& camera, const RestreamOptions& options);

// The mount path and public URL a restreamed camera is served on.
std::string mountPath(const std::string& cameraId);

}  // namespace visora::media
