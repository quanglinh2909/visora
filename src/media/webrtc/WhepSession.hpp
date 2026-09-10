#pragma once

// One browser watching one camera, over WHEP.
//
// WHEP is WebRTC-HTTP Egress: the browser POSTs an SDP offer, the server
// answers, and that is the whole signalling protocol — no websocket, no
// trickle, one request. The answer is sent after ICE gathering finishes, which
// costs a few hundred milliseconds and saves implementing PATCH at both ends.
// On a LAN, host candidates are available immediately and the wait is barely
// measurable.
//
// The session takes an EncodedSource, so live viewing and (step 7b) playback
// are the same class: everything difficult about WebRTC — SDP, SSRC, DTLS,
// payload types — exists once.
//
// Passthrough by default: depayloading and parsing already happened in the
// shared source, so a viewer costs one payloader and webrtcbin's SRTP. Adding
// viewers is nearly free, which is the entire reason the shared source exists.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "core/Result.hpp"
#include "media/source/EncodedSource.hpp"
#include "media/webrtc/WhepPipeline.hpp"

namespace visora::media {

struct WhepConfig {
    std::string stunServer;
    std::string turnServer;

    // How long to wait for ICE gathering before answering with what we have.
    // Non-trickle WHEP has to answer eventually, and a candidate that has not
    // arrived by now is usually one that never will.
    int iceGatherTimeoutMs = 5000;

    // How long a session may sit without ever connecting before it is given up
    // on. Once connected, liveness comes from webrtcbin's own peer connection
    // state rather than from a clock — a viewer watching quietly for an hour is
    // not idle, and a timer nobody feeds reaps it anyway.
    int connectTimeoutMs = 30000;
};

struct ViewerInfo {
    std::string sessionId;
    std::string cameraId;
    std::string codec;        // what is being SENT, which differs when transcoding
    bool transcoded = false;
    std::uint64_t rtpPackets = 0;
    std::int64_t startedAtMs = 0;

    // The browser's address as the HTTP server saw it. What an operator reads
    // to answer "who is watching this camera".
    std::string clientAddr;
    // Whether the peer connection has actually come up. An offer that never
    // connects is a session too, and it looks identical to a working one
    // without this.
    bool connected = false;
    // Set by WebRtcService: a session watching a recording, not a camera. The
    // session itself does not know which it is.
    bool playback = false;
};

class WhepSession {
public:
    WhepSession(std::string sessionId, std::string cameraId,
                std::shared_ptr<EncodedSource> source, WhepConfig config);
    ~WhepSession();

    WhepSession(const WhepSession&) = delete;
    WhepSession& operator=(const WhepSession&) = delete;

    // Negotiates and returns the SDP answer. Blocking — call it from a request
    // thread, never from a GStreamer thread.
    //
    // `clientAddressHint` is the browser's address as the HTTP server sees it,
    // used to replace an mDNS name in an ICE candidate. Chrome hides the local
    // IP behind "<uuid>.local", which libnice cannot resolve; the hint turns
    // such a candidate into one that can actually be reached. Optional: the
    // session still connects without it, because forcing ourselves into the
    // DTLS passive role means the browser speaks first.
    core::Result<std::string> start(const std::string& offerSdp,
                                    const std::string& clientAddressHint = {});

    void stop();

    bool alive() const;
    ViewerInfo info() const;

    const std::string& id() const { return m_sessionId; }

    // The GStreamer state. Public only because the bus, probe and signal
    // callbacks are free functions that receive it; nothing outside this
    // class's own translation unit names it.
    struct Impl;

private:
    // Everything between "the pipeline is playing" and "here is the answer".
    core::Result<std::string> negotiate(const std::string& offerSdp,
                                        const std::string& clientAddressHint);
    // Shortens libnice's STUN budget; see the constants in the .cpp.
    void tuneIceAgent();
    // Counts outgoing RTP and, when needed, rewrites the payload type byte.
    void installRtpProbe();

    std::string m_sessionId;
    std::string m_cameraId;
    std::shared_ptr<EncodedSource> m_source;
    WhepConfig m_config;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::media
