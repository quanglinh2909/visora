#pragma once

// The pipeline that sends one camera to one browser.
//
// Separated from the session so it can be asserted without a browser, a camera
// or a network. Almost every mistake possible here produces the same symptom —
// a session that reports "connected" and shows a black screen — so the launch
// string is worth checking directly.

#include <cstdint>
#include <string>

#include "media/gst/Codec.hpp"

namespace visora::media {

struct WhepPipelineOptions {
    // What the browser offered for the codec we are going to send.
    int payloadType = 96;

    // The synchronisation source, which must be the SAME number in the
    // payloader and in the caps. webrtcbin reads the SSRC FROM THE CAPS to
    // write "a=ssrc:" into the answer; if they disagree, Chrome receives and
    // decodes a stream it considers anonymous and the <video> track is never
    // connected to it. Everything reports connected and the screen stays black.
    std::uint32_t ssrc = 0;

    // Re-encode to H.264. Needed only for a browser that will not take the
    // camera's H.265; passthrough costs almost nothing and transcoding six
    // H.265 cameras cost the predecessor 168% of a core.
    bool transcode = false;

    std::string stunServer;
    std::string turnServer;

    // How much sending may lag before buffers are dropped rather than queued. A
    // viewer on a bad connection must not make the session grow without bound.
    int queueMs = 1000;
};

// The payload type the payloader actually runs at when the browser's number is
// outside the range the element can emit. See Sdp.hpp.
inline constexpr int kPayloaderInternalPayloadType = 96;

// Returns an empty string for a codec that cannot be sent.
//
// `sourceCodec` is what the camera produces. When `transcode` is set, the
// decoder and encoder come from the codec providers rather than being named
// here — which is what lets this same builder produce an MPP pipeline on a
// Rockchip board and a VA-API one on a workstation.
std::string whepLaunch(Codec sourceCodec, const WhepPipelineOptions& options);

// Whether the launch for these options needs the per-packet payload-type
// rewrite. Exposed because the session has to install a pad probe when it does.
bool needsPayloadTypeRewrite(const WhepPipelineOptions& options);

}  // namespace visora::media
