#pragma once

// A stream of parsed, still-encoded access units, delivered to one or more
// consumers.
//
// The port every consumer of camera video depends on: recording, WebRTC and the
// AI pipeline all want the same H.264/H.265 access units, and none of them
// should care whether they came from a camera over RTSP or from a file being
// played back. Two implementations differ only in WHERE the data comes from:
//
//   * RtspEncodedSource — pulls a camera, live, SHARED between every consumer
//     of that camera;
//   * (step 7) a playback source reading recorded segments, per session.
//
// Why shared matters, measured on the predecessor: a consumer that opens its
// own RTSP connection repeats the whole jitterbuffer + depayloader + parser
// chain for the same stream — about 15% of a core per consumer for 1080p. Ten
// viewers of one camera is ten times that work, and ten connections to a camera
// that permits four.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "media/gst/Codec.hpp"

typedef struct _GstBuffer GstBuffer;
typedef struct _GstCaps GstCaps;

namespace visora::media {

class EncodedSource {
public:
    // The buffer and caps are BORROWED: a consumer that wants to keep either
    // must ref it. Called on the source's streaming thread, so do little and
    // return — blocking here stalls every other consumer of this camera.
    using Sink = std::function<void(GstBuffer*, GstCaps*)>;

    virtual ~EncodedSource() = default;

    // Returns an id for removeSink. A new consumer starts receiving at the NEXT
    // KEYFRAME: handed a P-frame first, a decoder reconstructs it against
    // reference frames it never saw and produces visible garbage until the next
    // IDR — seconds of it, on a camera with a long GOP.
    virtual std::uint64_t addSink(Sink sink) = 0;
    virtual void removeSink(std::uint64_t id) = 0;

    // False once the source has failed or ended. A consumer should stop rather
    // than wait for data that is not coming.
    virtual bool alive() const = 0;

    virtual Codec codec() const = 0;

    // Measured bitrate of the stream, or 0 before there is enough to measure.
    //
    // Needed by anything that re-encodes: an encoder told to pick its own rate
    // guesses from resolution and frame rate, which for 1080p25 lands around
    // 6.5 Mbps regardless of what the camera is actually sending. Following the
    // source is the only way the output tracks the camera instead of a number
    // someone typed.
    virtual std::uint64_t bitrateBps() const { return 0; }
};

}  // namespace visora::media
