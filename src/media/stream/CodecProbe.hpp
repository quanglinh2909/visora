#pragma once

// Asks a camera what it is sending.
//
// A camera's codec is not configuration — it is whatever the device is set to,
// and it changes when someone reconfigures the camera. Probing rather than
// trusting a stored value is why a stream that switches from H264 to H265 keeps
// working instead of producing an undecodable feed.
//
// Blocking, with a timeout: it opens the RTSP session, reads the caps of the
// video stream and tears it down again. Call it off the request thread.

#include <string>

#include "core/Result.hpp"
#include "media/gst/Codec.hpp"

namespace visora::media {

struct ProbeOptions {
    int latencyMs = 300;
    // Long enough for a camera that is slow to answer DESCRIBE, short enough
    // that a dead address does not hold a retry slot for a minute.
    int timeoutMs = 7000;
};

// Unsupported: the camera answered and is sending something we cannot carry
// (MJPEG, MPEG4). Distinguished from NotFound/Internal on purpose — a wrong
// codec is permanent until someone reconfigures the camera, while a refused
// connection is worth retrying.
core::Result<Codec> probeRtspCodec(const std::string& url, const ProbeOptions& options = {});

}  // namespace visora::media
