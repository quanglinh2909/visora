#include "media/webrtc/WhepPipeline.hpp"

#include <algorithm>

#include "media/gst/CodecProvider.hpp"
#include "media/gst/LaunchPipeline.hpp"
#include "media/webrtc/Sdp.hpp"

namespace visora::media {

bool needsPayloadTypeRewrite(const WhepPipelineOptions& options) {
    return !payloadTypeIsEmittable(options.payloadType);
}

std::string whepLaunch(Codec sourceCodec, const WhepPipelineOptions& options) {
    if (sourceCodec == Codec::Unknown) return {};

    const Codec sendCodec = options.transcode ? Codec::H264 : sourceCodec;
    const bool rewritePt = needsPayloadTypeRewrite(options);
    const int payloaderPt = rewritePt ? kPayloaderInternalPayloadType : options.payloadType;

    LaunchPipeline pipeline;
    LaunchChain& chain = pipeline.chain();

    // The source is the shared camera source pushing parsed access units, not
    // an rtspsrc of this session's own — otherwise every viewer of one camera
    // repeats the jitterbuffer, depayloader and parser for the same stream.
    //
    // do-timestamp=true: the buffers arrive carrying the SOURCE pipeline's
    // clock, which is a different clock. Left alone, the payloader computes RTP
    // timestamps from it that mean nothing to the browser, which then cannot
    // reassemble the stream — connected, and black.
    chain.add(ElementSpec("appsrc")
                  .named("src")
                  .set("is-live", true)
                  .set("format", "time")
                  .set("do-timestamp", true)
                  .set("max-bytes", 0)
                  .set("block", false))
        .caps(parsedCaps(sourceCodec));

    if (options.transcode) {
        // Through the codec providers, so this is MPP on a Rockchip board,
        // NVDEC/NVENC on an NVIDIA machine and libav where there is nothing
        // else. The predecessor named mppvideodec and mpph264enc here, which is
        // why it only ran on one board.
        const auto decoder = resolveDecoder(sourceCodec);
        EncoderParams params;
        params.lowLatency = true;
        // -1 means "one keyframe per second" for the encoders that take it. A
        // viewer joining sees a picture in about a second even when the camera
        // itself uses a GOP of tens of seconds — a real benefit of this path.
        params.gopSize = -1;
        const auto encoder = resolveEncoder(Codec::H264, params);
        if (!decoder || !encoder) return {};

        chain.add(ElementSpec(parser(sourceCodec)).set("config-interval", -1))
            .add(decoder->spec)
            // videoconvert between the decoder and the encoder's format, and it
            // is not belt-and-braces.
            //
            // A hardware decoder does not necessarily produce what the encoder
            // wants, and often cannot be asked to: linking vah265dec straight to
            // "video/x-raw,format=I420" fails outright with "can't handle caps",
            // because VA-API decodes into its own memory. Where the formats
            // already agree — mppvideodec to mpph264enc, both NV12 — videoconvert
            // negotiates to passthrough and costs nothing, so this does not give
            // up the zero-copy path it exists to protect.
            .add("videoconvert")
            .caps("video/x-raw,format=" + encoderInputFormatFor(Codec::H264, params))
            .add(encoder->spec)
            .add(ElementSpec(parser(Codec::H264)).set("config-interval", -1));
    }

    ElementSpec pay(payloader(sendCodec));
    pay.named("pay")
        .set("pt", payloaderPt)
        .set("ssrc", static_cast<long long>(options.ssrc))
        .set("config-interval", -1);
    // aggregate-mode decides how NAL units are packed.
    //
    // H.264 uses zero-latency: each NAL goes out as it appears rather than
    // being gathered per access unit, which is what keeps latency under a
    // second. H.265 uses none, because zero-latency packs VPS/SPS/PPS into an
    // Aggregation Packet, and that is the point HEVC decoders disagree about
    // most. "none" sends single NALs and fragments, which all of them accept,
    // and costs no extra latency.
    pay.set("aggregate-mode", sendCodec == Codec::H265 ? "none" : "zero-latency");
    chain.add(pay);

    if (rewritePt) {
        // capssetter, not a capsfilter: a capsfilter cannot be linked straight
        // to the payloader because it would have to negotiate the payloader's
        // own [96,127] pad template. capssetter has an ANY template and gets
        // between them.
        //
        // join=true, replace=false: override only the payload field and leave
        // the media, encoding-name, clock-rate and ssrc the payloader set.
        chain.add(ElementSpec("capssetter")
                      .set("join", true)
                      .set("replace", false)
                      .set("caps", "application/x-rtp,payload=(int)" +
                                       std::to_string(options.payloadType)));
    }

    // The full caps, even with a capssetter in front, and this is load-bearing.
    //
    // webrtcbin reads the caps on its sink pad WHEN IT BUILDS THE ANSWER, to
    // write "a=ssrc:" and to fix the SSRC it will send with. A capssetter's
    // caps only exist once data flows, which is after the answer — so with only
    // a capssetter the answer carries no a=ssrc line at all and webrtcbin picks
    // its own. Chrome then receives packets, decodes them, and never attaches
    // the track to the <video> element: black screen with every state reporting
    // connected.
    chain.caps("application/x-rtp,media=video,encoding-name=" +
               std::string(encodingName(sendCodec)) + ",payload=" +
               std::to_string(options.payloadType) + ",clock-rate=90000,ssrc=(uint)" +
               std::to_string(options.ssrc));

    // A queue to put a thread boundary here, so the shared source is never held
    // up by this viewer's sending — and bounded in time, so a viewer too slow
    // to keep up drops frames instead of growing the process.
    chain.add(ElementSpec("queue")
                  .set("max-size-buffers", 0)
                  .set("max-size-bytes", 0)
                  .set("max-size-time",
                       static_cast<long long>(std::max(1, options.queueMs)) * 1'000'000LL));

    ElementSpec webrtc("webrtcbin");
    webrtc.named("webrtc").set("bundle-policy", "max-bundle").set("latency", 0);
    if (!options.stunServer.empty()) webrtc.setQuoted("stun-server", options.stunServer);
    if (!options.turnServer.empty()) webrtc.setQuoted("turn-server", options.turnServer);
    chain.add(webrtc);

    // NOT wrapped: gst_parse_launch wants an unparenthesised description.
    return pipeline.toLaunch(/*wrapped=*/false);
}

}  // namespace visora::media
