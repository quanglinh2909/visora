#pragma once

// The parts of SDP negotiation that are decisions rather than plumbing.
//
// Text in, text out, no GStreamer: every rule here is one that produces a
// "connected" WebRTC session showing a black screen when it is wrong, and a
// black screen is not something a test can catch by watching. Parsed as lines
// because SDP IS lines — pulling in a parser would not make any of this simpler
// and would put GStreamer in a header that does not need it.

#include <string>
#include <vector>

namespace visora::media {

// A payload type is what an RTP packet says it is carrying, and both ends must
// agree on the number.
//
// rtph264pay and rtph265pay can only EMIT a payload type in the dynamic range
// [96, 127] — that is their src pad template, not a convention. Recent Chrome
// has exhausted that range and offers additional codecs in the 35..63 block; on
// Windows, where it has hardware HEVC decoding, it offers "a=rtpmap:49 H265".
//
// Pinning 49 into the pipeline fails at construction ("can't handle caps ...
// payload=(int)49"), and setting the `pt` property instead is silently ignored
// — the element still emits 96, the browser sees a number that is not the one
// in the answer, and drops every packet.
inline constexpr int kMinDynamicPayloadType = 96;
inline constexpr int kMaxDynamicPayloadType = 127;

inline bool payloadTypeIsEmittable(int payloadType) {
    return payloadType >= kMinDynamicPayloadType && payloadType <= kMaxDynamicPayloadType;
}

// The payload type the browser offered for a codec, or -1 if it offered none.
//
// `codec` is "H264" or "H265", matched case-insensitively against the rtpmap
// entries of the first video m-line.
//
// For H.264 the packetization-mode=1 variant is preferred: that is FU-A
// fragmentation, which every browser supports and which is what rtph264pay
// produces. H.265 has no equivalent parameter, so the first entry wins.
//
// A number OUTSIDE the emittable range is still returned — the rewrite path
// handles it — but an in-range one is preferred, because that path is simpler.
int pickPayloadType(const std::string& offerSdp, const std::string& codec);

// Rewrites every "a=setup:actpass" to "a=setup:active" in the OFFER.
//
// The offer, not the answer. webrtcbin infers its own DTLS role from what the
// remote description says, so editing the answer changes the text and not the
// behaviour: the wording says one thing while webrtcbin still runs as a client,
// both ends try to be client, and the handshake never completes.
//
// Returned unchanged when the browser already committed to a role.
std::string forceRemoteDtlsActive(const std::string& offerSdp);

// One ICE candidate from the offer, with the m-line it belongs to.
struct RemoteCandidate {
    int mlineIndex = 0;
    std::string candidate;  // the value after "a=candidate:", as webrtcbin wants it
};

// The candidates in the offer.
//
// webrtcbin does NOT pick these up from the remote description — they have to
// be handed to it one by one with add-ice-candidate. Skip this and it knows no
// address for the browser at all, so nothing ever connects.
std::vector<RemoteCandidate> remoteCandidates(const std::string& offerSdp);

// Whether the offer is one we can answer at all: it must contain a video
// m-line. Checked before building anything, so a malformed request is a 400
// rather than a pipeline that fails obscurely later.
bool hasVideoMedia(const std::string& offerSdp);

}  // namespace visora::media
