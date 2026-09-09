// WebRTC negotiation: the decisions, not the plumbing.
//
// Almost every mistake possible in here produces exactly one symptom — a
// session that reports "connected" and shows a black screen — which is
// impossible to diagnose by watching and easy to assert directly.

#include "TestHarness.hpp"

#include <string>

#include "media/webrtc/Sdp.hpp"
#include "media/webrtc/WhepPipeline.hpp"

using namespace visora::media;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// What Chrome sends: several codecs, H.264 at more than one payload type, only
// one of them with the packetization mode the payloader produces.
const char* kChromeOffer =
    "v=0\r\n"
    "o=- 1 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=recvonly\r\n"
    "a=rtpmap:96 VP8/90000\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=rtpmap:102 H264/90000\r\n"
    "a=fmtp:102 packetization-mode=0;profile-level-id=42001f\r\n"
    "a=rtpmap:103 H264/90000\r\n"
    "a=fmtp:103 packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=candidate:1 1 udp 2113937151 192.168.1.5 55000 typ host\r\n"
    "a=candidate:2 1 udp 2113937150 abc-123.local 55001 typ host\r\n";

}  // namespace

VS_TEST(the_payload_type_comes_from_the_offer_not_from_us) {
    // As the ANSWERING side we must send with the number the offer assigned.
    // Pinning our own leaves our caps unable to intersect the offer: webrtcbin
    // quietly drops the video, makes an empty recvonly transceiver, and leaves
    // the data probe in place — so the browser's DTLS ClientHello sits in a
    // queue and the session hangs at "connecting", a protocol layer away from
    // the cause.
    VS_CHECK_EQ(pickPayloadType(kChromeOffer, "H264"), 103);
    // The offer has no H.265, and saying so is what triggers transcoding.
    VS_CHECK_EQ(pickPayloadType(kChromeOffer, "H265"), -1);
}

VS_TEST(h264_prefers_the_packetization_mode_the_payloader_produces) {
    // 102 comes first but is mode 0. rtph264pay emits FU-A, which is mode 1, so
    // answering with 102 describes packets we do not send.
    VS_CHECK_EQ(pickPayloadType(kChromeOffer, "H264"), 103);
}

VS_TEST(a_payload_type_outside_the_emittable_range_is_still_reported) {
    // Chrome on Windows, where it has hardware HEVC, offers H.265 at 49 —
    // outside [96,127], which is what rtph265pay's pad template allows. It must
    // still be reported, because the rewrite path can carry it; pretending the
    // browser offered nothing would force a needless transcode.
    const char* offer =
        "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 49\r\n"
        "a=rtpmap:49 H265/90000\r\n";
    VS_CHECK_EQ(pickPayloadType(offer, "H265"), 49);
    VS_CHECK(!payloadTypeIsEmittable(49));
    VS_CHECK(payloadTypeIsEmittable(96));
    VS_CHECK(payloadTypeIsEmittable(127));
    VS_CHECK(!payloadTypeIsEmittable(128));
}

VS_TEST(an_in_range_payload_type_is_preferred_over_an_out_of_range_one) {
    // Both work, but the in-range one needs no per-packet rewriting.
    const char* offer =
        "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 49 100\r\n"
        "a=rtpmap:49 H265/90000\r\n"
        "a=rtpmap:100 H265/90000\r\n";
    VS_CHECK_EQ(pickPayloadType(offer, "H265"), 100);
}

VS_TEST(only_the_first_video_m_line_is_considered) {
    // An audio m-line after the video one must not contribute payload types.
    const char* offer =
        "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 100\r\n"
        "a=rtpmap:100 H264/90000\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=rtpmap:111 H264/90000\r\n";
    VS_CHECK_EQ(pickPayloadType(offer, "H264"), 100);
}

VS_TEST(the_browser_is_pinned_to_the_dtls_active_role) {
    // So webrtcbin takes passive and the BROWSER sends the first ClientHello.
    // As the client we would have to send it to an address hidden behind an
    // mDNS name libnice cannot resolve — into nothing — and the handshake hangs
    // for ever while ICE reports completed.
    const std::string patched = forceRemoteDtlsActive(kChromeOffer);
    VS_CHECK(contains(patched, "a=setup:active"));
    VS_CHECK(!contains(patched, "a=setup:actpass"));

    // A browser that already chose is left alone.
    const std::string chosen = "a=setup:passive\r\n";
    VS_CHECK(forceRemoteDtlsActive(chosen) == chosen);
}

VS_TEST(every_candidate_in_the_offer_is_extracted_with_its_m_line) {
    // webrtcbin does NOT read these out of the remote description; they must be
    // handed to it one at a time. Without them it knows no address for the
    // browser: signalling completes and ICE sits at checking for ever.
    const auto candidates = remoteCandidates(kChromeOffer);
    VS_CHECK_EQ(candidates.size(), std::size_t{2});
    if (candidates.size() == 2) {
        VS_CHECK_EQ(candidates[0].mlineIndex, 0);
        // "candidate:..." with the "a=" removed, which is the form the signal
        // expects.
        VS_CHECK(candidates[0].candidate.rfind("candidate:", 0) == 0);
        VS_CHECK(contains(candidates[1].candidate, ".local"));
    }
}

VS_TEST(a_malformed_offer_is_rejected_before_anything_is_built) {
    VS_CHECK(!hasVideoMedia("v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"));
    VS_CHECK(!hasVideoMedia(""));
    VS_CHECK(hasVideoMedia(kChromeOffer));
}

// --- the pipeline ------------------------------------------------------------

VS_TEST(the_ssrc_is_pinned_in_both_the_payloader_and_the_caps) {
    // webrtcbin reads the SSRC FROM THE CAPS to write "a=ssrc:" into the
    // answer. If the payloader picks its own, Chrome receives and decodes a
    // stream it considers anonymous while the <video> track waits for the SSRC
    // that was announced — black screen, every state "connected".
    WhepPipelineOptions options;
    options.payloadType = 103;
    options.ssrc = 123456;
    const std::string launch = whepLaunch(Codec::H264, options);

    VS_CHECK(contains(launch, "ssrc=123456"));
    VS_CHECK(contains(launch, "ssrc=(uint)123456"));
    // The full caps must be there even so: they are what exists at answer time.
    VS_CHECK(contains(launch, "application/x-rtp,media=video,encoding-name=H264,payload=103"));
    VS_CHECK(contains(launch, "clock-rate=90000"));
}

VS_TEST(an_out_of_range_payload_type_gets_a_capssetter_and_a_rewrite) {
    WhepPipelineOptions options;
    options.payloadType = 49;   // Chrome on Windows, H.265
    options.ssrc = 7;
    const std::string launch = whepLaunch(Codec::H265, options);

    VS_CHECK(needsPayloadTypeRewrite(options));
    // The payloader runs at a number it is allowed to emit...
    VS_CHECK(contains(launch, "pt=96"));
    // ...capssetter corrects the caps so the ANSWER matches the offer...
    VS_CHECK(contains(launch, "capssetter"));
    VS_CHECK(contains(launch, "payload=(int)49"));
    // ...and the final caps announce the offered number.
    VS_CHECK(contains(launch, "payload=49"));

    // An in-range one needs none of that.
    options.payloadType = 100;
    VS_CHECK(!needsPayloadTypeRewrite(options));
    VS_CHECK(!contains(whepLaunch(Codec::H265, options), "capssetter"));
}

VS_TEST(h265_and_h264_use_different_aggregate_modes) {
    WhepPipelineOptions options;
    options.payloadType = 100;
    // H.264: zero-latency pushes each NAL as it appears, which is what keeps
    // latency under a second.
    VS_CHECK(contains(whepLaunch(Codec::H264, options), "aggregate-mode=zero-latency"));
    // H.265: none. zero-latency packs VPS/SPS/PPS into an Aggregation Packet,
    // the thing HEVC decoders disagree about most, and costs no less latency.
    VS_CHECK(contains(whepLaunch(Codec::H265, options), "aggregate-mode=none"));
}

VS_TEST(the_source_timestamps_are_replaced_not_kept) {
    // The buffers carry the SOURCE pipeline's clock. Left alone the payloader
    // computes RTP timestamps that mean nothing to the browser.
    WhepPipelineOptions options;
    options.payloadType = 100;
    VS_CHECK(contains(whepLaunch(Codec::H264, options), "do-timestamp=true"));
}

VS_TEST(an_unknown_codec_produces_no_pipeline_rather_than_a_broken_one) {
    WhepPipelineOptions options;
    VS_CHECK(whepLaunch(Codec::Unknown, options).empty());
}

VS_TEST(the_transcode_path_converts_between_the_decoder_and_the_encoder) {
    // A hardware decoder does not necessarily produce what the encoder wants,
    // and often cannot be asked to: vah265dec linked straight to
    // "video/x-raw,format=I420" fails with "can't handle caps", because VA-API
    // decodes into its own memory. This was a 500 on every H.265 camera whose
    // viewer could not take H.265.
    WhepPipelineOptions options;
    options.payloadType = 100;
    options.transcode = true;
    const std::string launch = whepLaunch(Codec::H265, options);
    if (launch.empty()) {
        // No decoder or encoder on this machine; nothing to assert.
        std::fprintf(stderr, "    (no transcode elements here; skipped)\n");
        return;
    }
    VS_CHECK(contains(launch, "videoconvert"));
    // And it still sends H.264, at the payload type the browser offered.
    VS_CHECK(contains(launch, "encoding-name=H264,payload=100"));
    VS_CHECK(contains(launch, "rtph264pay"));
}

VS_MAIN()
