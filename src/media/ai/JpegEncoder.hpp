#pragma once

// Encodes one decoded frame to JPEG, for the result a consumer receives.
//
// A PERSISTENT pipeline, not one per frame. Building and tearing down a
// GStreamer pipeline costs milliseconds; a busy install produces several
// results a second per camera, and paying that each time is most of the cost of
// producing a result at all.
//
// The encoder itself comes from the codec providers, so this uses hardware JPEG
// where a board has it and libjpeg where it does not.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Image.hpp"
#include "core/Result.hpp"

namespace visora::media {

class JpegEncoder {
public:
    explicit JpegEncoder(int quality = 85);
    ~JpegEncoder();

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    core::Status start();
    void stop();

    // Blocking, and expected to take a few milliseconds. Call it from a job
    // worker, never from a decoder's streaming thread.
    core::Result<std::vector<std::uint8_t>> encode(const core::ImageView& frame);

    static std::string launchFor(int quality);

private:
    struct Impl;

    int m_quality;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::media
