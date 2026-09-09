#pragma once

// Decodes an uploaded image, so a model can be tried without a camera.
//
// The mirror of JpegEncoder and for the same reason: a persistent pipeline,
// because building one per request costs milliseconds and an operator tuning a
// model runs this repeatedly.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Image.hpp"
#include "core/Result.hpp"

namespace visora::media {

class JpegDecoder {
public:
    JpegDecoder();
    ~JpegDecoder();

    JpegDecoder(const JpegDecoder&) = delete;
    JpegDecoder& operator=(const JpegDecoder&) = delete;

    core::Status start();
    void stop();

    // Decodes to packed RGB888. Blocking; call it from a request thread.
    core::Result<core::OwnedImage> decode(const std::uint8_t* bytes, std::size_t size);

    static std::string launch();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::media
