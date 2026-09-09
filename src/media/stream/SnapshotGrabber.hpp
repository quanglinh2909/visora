#pragma once

// One JPEG frame from a camera, on demand.
//
// A short-lived pipeline per call: connect, decode a few frames, encode one,
// tear everything down. Nothing is kept open between calls, because a snapshot
// is rare and holding a second RTSP session per camera open on the chance
// someone asks costs more than the occasional reconnect.
//
// An interface rather than a free function so an endpoint that serves snapshots
// can be tested without a camera on the network.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Result.hpp"

namespace visora::media {

struct SnapshotOptions {
    // Jitter-buffer latency. There is a floor below this, applied in the
    // implementation — see kMinimumLatencyMs.
    int latencyMs = 500;
    int timeoutMs = 12000;
    int quality = 85;

    // Frames to decode and throw away before keeping one.
    //
    // Hardware decoders emit a partly-built first frame — on Rockchip MPP with
    // H.265 it comes out uniformly green — for the first fraction of a second
    // after the pipeline opens. Pulling frame zero gets that one. With
    // max-buffers=1 drop=false each pull advances the decoder exactly one
    // frame, so this is a duration in frames rather than a sleep: ~0.3 s at
    // 25 fps, and correct at any frame rate.
    int warmupFrames = 8;
};

// Below this, a 1080p H.265 keyframe arriving as one burst of RTP packets does
// not fit in the jitter buffer, the tail is dropped, and the decoder turns the
// incomplete IDR into a green frame. Measured on RK3588: the same pipeline at
// 200 ms is green and at 500 ms is correct. Applies to snapshots for the same
// reason it applies to the restream (RestreamOptions::kMinimumLatencyMs).
inline constexpr int kSnapshotMinimumLatencyMs = 500;

class SnapshotGrabber {
public:
    virtual ~SnapshotGrabber() = default;

    // JPEG bytes, or why not. Blocking for up to `timeoutMs`: call it from a
    // request thread, never from a GLib main loop.
    virtual core::Result<std::vector<std::uint8_t>> grab(const std::string& rtspUrl,
                                                         const SnapshotOptions& options) = 0;
};

// The launch description, separated from the running of it so the pipeline this
// builds is asserted on a machine with no camera and no plugins.
//
// decodebin rather than a decoder resolved from the codec: a snapshot has not
// probed the stream and does not know whether it is H.264 or H.265. decodebin
// picks by rank, which lands on the hardware decoder when one is installed.
//
// The JPEG encoder DOES come from the codec providers, through
// resolveJpegEncoder — which walks every provider rather than asking the best
// one, because the best one on Rockchip deliberately offers no JPEG encoder.
std::string snapshotLaunch(const std::string& rtspUrl, const SnapshotOptions& options);

// The GStreamer implementation.
std::shared_ptr<SnapshotGrabber> makeGstSnapshotGrabber();

}  // namespace visora::media
