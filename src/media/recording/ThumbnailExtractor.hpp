#pragma once

// One JPEG from a point inside a recorded segment — the preview a timeline
// shows as an operator scrubs it.
//
// Same idea as the live snapshot but against a file: open a short-lived decode
// pipeline, seek to a keyframe before the requested instant, decode forward
// until a real frame appears, encode it, tear everything down.
//
// An interface for the same reason SnapshotGrabber is one: the endpoint that
// serves thumbnails is then testable without a recording on disk.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "media/gst/Codec.hpp"

namespace visora::media {

struct ThumbnailOptions {
    // Output width; the height follows the source aspect with square pixels.
    int width = 320;
    int quality = 70;
};

class ThumbnailExtractor {
public:
    virtual ~ThumbnailExtractor() = default;

    // `offsetMs` is measured from the start of the file, not from the epoch.
    virtual core::Result<std::vector<std::uint8_t>> extract(const std::string& path, Codec codec,
                                                            std::int64_t offsetMs,
                                                            const ThumbnailOptions& options) = 0;
};

// How far BEFORE the requested instant to begin decoding.
//
// A camera using gradual refresh (Dahua's "smart codec", H.265+) has no clean
// IDR to seek to: after each recovery point the decoder needs a full refresh
// cycle — fifteen or so frames — before it has a complete picture. Landing
// exactly on the instant lands in the middle of such a cycle and yields a
// half-built grey frame. Starting two seconds early lets the cycle finish
// before the point that was asked for. Two seconds of error is invisible in a
// scrub preview.
inline constexpr std::int64_t kThumbnailRunbackMs = 2000;

std::string thumbnailLaunch(const std::string& path, Codec codec,
                            const ThumbnailOptions& options);

std::shared_ptr<ThumbnailExtractor> makeGstThumbnailExtractor();

}  // namespace visora::media
