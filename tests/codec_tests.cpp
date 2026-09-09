// Pipeline construction and codec-provider selection.
//
// None of this needed a camera or a plugin to write, and none of it needs one
// to run: the element probe is a seam, so "prefer MPP, fall back to VA-API,
// then software" is exercised for hardware this machine does not have. In the
// predecessor not one line of pipeline construction was covered by a test,
// because the strings only existed at run time on a board with a camera
// attached.

#include "TestHarness.hpp"

#include <memory>
#include <set>
#include <string>

#include "media/gst/CodecProvider.hpp"
#include "media/gst/ElementAvailability.hpp"
#include "media/gst/LaunchPipeline.hpp"

using namespace visora;
using visora::media::Codec;
using visora::media::ElementSpec;
using visora::media::EncoderParams;
using visora::media::LaunchPipeline;

namespace {

// Pretends a fixed set of elements is installed, so provider preference can be
// tested for machines we do not have.
void pretendInstalled(std::set<std::string> elements) {
    media::setElementProbe(
        [installed = std::move(elements)](const std::string& name) {
            return installed.count(name) > 0;
        });
}

void restoreRealProbe() { media::setElementProbe(nullptr); }

// The provider the registry would pick, given what is pretended installed.
std::string selectedProviderId() {
    auto selected = media::codecProviderRegistry().select("codec");
    return selected.ok() ? std::string(selected.value()->id()) : std::string();
}

std::unique_ptr<media::CodecProvider> providerById(const char* id) {
    auto selected = media::codecProviderRegistry().select("codec", id);
    if (!selected.ok()) return nullptr;
    return std::move(selected.value());
}

}  // namespace

// --- element rendering -------------------------------------------------------

VS_TEST(an_element_renders_its_name_first_then_properties_in_order) {
    const std::string launch = ElementSpec("mpph264enc")
                                   .named("enc")
                                   .set("gop", -1)
                                   .set("rc-mode", "vbr")
                                   .set("bps", 4000000LL)
                                   .toLaunch();
    // Order is stable so golden strings are possible at all.
    VS_CHECK(launch == "mpph264enc name=enc gop=-1 rc-mode=vbr bps=4000000");
}

VS_TEST(setting_a_property_twice_replaces_it_without_reordering) {
    const std::string launch =
        ElementSpec("x264enc").set("bitrate", 2000).set("tune", "zerolatency").set("bitrate", 4000).toLaunch();
    VS_CHECK(launch == "x264enc bitrate=4000 tune=zerolatency");
}

VS_TEST(quoted_values_escape_what_the_launch_parser_would_eat) {
    // An RTSP URL with a password containing a quote is not hypothetical.
    const std::string launch =
        ElementSpec("rtspsrc")
            .named("src")
            .setQuoted("location", "rtsp://user:pa\"ss@10.0.0.1/stream")
            .set("protocols", "tcp")
            .toLaunch();
    VS_CHECK(launch ==
             "rtspsrc name=src location=\"rtsp://user:pa\\\"ss@10.0.0.1/stream\" protocols=tcp");
}

VS_TEST(booleans_render_as_gstreamer_spells_them) {
    VS_CHECK(ElementSpec("appsrc").set("is-live", true).toLaunch() == "appsrc is-live=true");
    VS_CHECK(ElementSpec("appsrc").set("block", false).toLaunch() == "appsrc block=false");
}

VS_TEST(an_empty_element_renders_to_nothing_and_is_skipped) {
    ElementSpec empty;
    VS_CHECK(!empty.valid());
    VS_CHECK(empty.toLaunch().empty());

    LaunchPipeline pipeline;
    pipeline.chain().add(ElementSpec("a")).add(empty).add(ElementSpec("b"));
    // A provider returning nothing must not leave a dangling " ! ".
    VS_CHECK(pipeline.toLaunch() == "a ! b");
}

// --- pipeline rendering ------------------------------------------------------

VS_TEST(a_chain_joins_elements_and_caps_with_the_link_operator) {
    LaunchPipeline pipeline;
    pipeline.chain()
        .add(ElementSpec("rtspsrc").named("src").setQuoted("location", "rtsp://cam/1"))
        .caps("application/x-rtp,media=video,encoding-name=H264")
        .add("rtph264depay")
        .add(ElementSpec("h264parse").set("config-interval", -1));

    VS_CHECK(pipeline.toLaunch() ==
             "rtspsrc name=src location=\"rtsp://cam/1\" ! "
             "application/x-rtp,media=video,encoding-name=H264 ! "
             "rtph264depay ! h264parse config-interval=-1");
}

VS_TEST(wrapping_in_parentheses_is_a_deliberate_choice_not_a_default) {
    // gst_parse_launch returns a real GstPipeline only for an UNWRAPPED
    // description; gst_rtsp_media_factory_set_launch wants it wrapped. Getting
    // this backwards yields a GstBin where a GstPipeline was expected, and the
    // failure shows up far from the cause.
    LaunchPipeline pipeline;
    pipeline.chain().add("videotestsrc").add("fakesink");

    VS_CHECK(pipeline.toLaunch(false) == "videotestsrc ! fakesink");
    VS_CHECK(pipeline.toLaunch(true) == "( videotestsrc ! fakesink )");
}

VS_TEST(tee_branches_render_as_gstreamer_expects) {
    LaunchPipeline pipeline;
    pipeline.chain()
        .add(ElementSpec("appsrc").named("record_src").set("is-live", true))
        .caps("video/x-h264,stream-format=byte-stream,alignment=au")
        .add(ElementSpec("tee").named("record_t"));
    pipeline.branch("record_t").add("queue").add(ElementSpec("splitmuxsink").named("record_sink"));
    pipeline.branch("record_t").add("queue").add("fakesink");

    VS_CHECK(pipeline.toLaunch() ==
             "appsrc name=record_src is-live=true ! "
             "video/x-h264,stream-format=byte-stream,alignment=au ! "
             "tee name=record_t "
             "record_t. ! queue ! splitmuxsink name=record_sink "
             "record_t. ! queue ! fakesink");
}

// --- provider selection ------------------------------------------------------

VS_TEST(a_rockchip_board_selects_mpp) {
    pretendInstalled({"mppvideodec", "mpph264enc", "avdec_h264", "x264enc", "jpegenc"});
    VS_CHECK(selectedProviderId() == "rockchip-mpp");
    restoreRealProbe();
}

VS_TEST(an_nvidia_machine_selects_nvcodec_over_vaapi_and_software) {
    pretendInstalled({"nvh264dec", "nvh264enc", "vah264dec", "avdec_h264", "x264enc"});
    VS_CHECK(selectedProviderId() == "nvidia");
    restoreRealProbe();
}

VS_TEST(an_intel_machine_selects_vaapi) {
    pretendInstalled({"vah264dec", "vah264enc", "avdec_h264", "x264enc"});
    VS_CHECK(selectedProviderId() == "vaapi");
    restoreRealProbe();
}

VS_TEST(a_plain_arm_board_selects_v4l2) {
    pretendInstalled({"v4l2h264dec", "v4l2h264enc", "avdec_h264", "x264enc"});
    VS_CHECK(selectedProviderId() == "v4l2");
    restoreRealProbe();
}

VS_TEST(a_machine_with_no_video_hardware_selects_software) {
    pretendInstalled({"avdec_h264", "avdec_h265", "x264enc", "jpegenc"});
    VS_CHECK(selectedProviderId() == "software");
    restoreRealProbe();
}

VS_TEST(a_machine_with_nothing_installed_says_so_rather_than_guessing) {
    pretendInstalled({});
    auto selected = media::codecProviderRegistry().select("codec");
    VS_CHECK(!selected.ok());
    // The reason must name what was missing, not just "unavailable".
    VS_CHECK(selected.error().message.find("avdec_h264") != std::string::npos);
    restoreRealProbe();
}

// --- what each provider produces ---------------------------------------------

VS_TEST(rockchip_encoder_gets_bits_per_second_and_follows_source_keyframes) {
    pretendInstalled({"mppvideodec", "mpph264enc"});
    auto provider = providerById("rockchip-mpp");
    VS_CHECK(provider != nullptr);
    if (!provider) { restoreRealProbe(); return; }

    EncoderParams params;
    params.bitrateKbps = 4000;
    params.gopSize = -1;
    const auto encoder = provider->encoder(Codec::H264, params);
    VS_CHECK(encoder.has_value());
    if (encoder) {
        // bps is bits per second, not kbit/s. Left at 0 MPP estimates
        // w*h*fps/8, several times what the source actually uses.
        VS_CHECK(encoder->valueOf("bps") == "4000000");
        VS_CHECK(encoder->valueOf("gop") == "-1");
    }
    // NV12 in, or the encoder converts on the CPU and the point is lost.
    VS_CHECK(provider->encoderInputFormat() == "NV12");
    restoreRealProbe();
}

VS_TEST(software_encoder_gets_kilobits_because_x264enc_counts_differently) {
    pretendInstalled({"avdec_h264", "x264enc"});
    auto provider = providerById("software");
    VS_CHECK(provider != nullptr);
    if (!provider) { restoreRealProbe(); return; }

    EncoderParams params;
    params.bitrateKbps = 4000;
    const auto encoder = provider->encoder(Codec::H264, params);
    VS_CHECK(encoder.has_value());
    if (encoder) VS_CHECK(encoder->valueOf("bitrate") == "4000");
    VS_CHECK(provider->encoderInputFormat() == "I420");
    restoreRealProbe();
}

VS_TEST(rockchip_declines_hardware_jpeg_on_purpose) {
    pretendInstalled({"mppvideodec", "mpph264enc", "mppjpegenc"});
    auto provider = providerById("rockchip-mpp");
    VS_CHECK(provider != nullptr);
    if (!provider) { restoreRealProbe(); return; }
    // Even with mppjpegenc installed. It produces green frames for some inputs
    // and has been seen to fault its core hard enough to freeze the board;
    // software JPEG costs milliseconds, the hardware path costs correctness.
    VS_CHECK(!provider->jpegEncoder(85).has_value());
    restoreRealProbe();
}

VS_TEST(a_provider_declines_a_codec_whose_element_is_absent) {
    // H265 encode without mpph265enc installed.
    pretendInstalled({"mppvideodec", "mpph264enc"});
    auto provider = providerById("rockchip-mpp");
    VS_CHECK(provider != nullptr);
    if (!provider) { restoreRealProbe(); return; }
    VS_CHECK(provider->encoder(Codec::H264, EncoderParams{}).has_value());
    VS_CHECK(!provider->encoder(Codec::H265, EncoderParams{}).has_value());
    restoreRealProbe();
}

VS_TEST(callers_needing_one_capability_can_walk_every_available_provider) {
    // The best provider may not offer a JPEG encoder — Rockchip deliberately
    // does not. A caller must be able to reach past it to software rather than
    // giving up.
    pretendInstalled({"mppvideodec", "mpph264enc", "avdec_h264", "x264enc", "jpegenc"});
    bool foundJpeg = false;
    for (media::CodecProvider* provider : media::availableCodecProviders()) {
        if (provider->jpegEncoder(85).has_value()) foundJpeg = true;
    }
    VS_CHECK(foundJpeg);
    restoreRealProbe();
}

// --- resolving a role across providers ---------------------------------------
//
// The case that motivated these: the development machine has VA-API decode and
// no VA-API encoder. Selecting the best provider and asking it for an encoder
// fails on a machine that can encode perfectly well with the next one down.

VS_TEST(a_decode_only_accelerator_does_not_break_encoding) {
    pretendInstalled({"vah264dec", "avdec_h264", "x264enc"});  // no vah264enc

    const auto decoder = media::resolveDecoder(Codec::H264);
    VS_CHECK(decoder.has_value());
    if (decoder) {
        VS_CHECK(decoder->providerId == "vaapi");        // hardware decode
        VS_CHECK(decoder->spec.factory() == "vah264dec");
    }

    const auto encoder = media::resolveEncoder(Codec::H264, EncoderParams{});
    VS_CHECK(encoder.has_value());
    if (encoder) {
        VS_CHECK(encoder->providerId == "software");      // and software encode
        VS_CHECK(encoder->spec.factory() == "x264enc");
    }
    restoreRealProbe();
}

VS_TEST(jpeg_falls_through_rockchip_to_software) {
    // Rockchip declines JPEG on purpose; a board must still take snapshots.
    pretendInstalled({"mppvideodec", "mpph264enc", "mppjpegenc", "avdec_h264", "x264enc",
                      "jpegenc"});
    const auto jpeg = media::resolveJpegEncoder(85);
    VS_CHECK(jpeg.has_value());
    if (jpeg) {
        VS_CHECK(jpeg->providerId == "software");
        VS_CHECK(jpeg->spec.factory() == "jpegenc");
        VS_CHECK(jpeg->spec.valueOf("quality") == "85");
    }
    restoreRealProbe();
}

VS_TEST(the_input_format_comes_from_whoever_supplies_the_encoder) {
    // Pairing one provider's encoder with another's input format would make the
    // encoder convert on the CPU, silently undoing the acceleration.
    pretendInstalled({"mppvideodec", "mpph264enc"});
    VS_CHECK(media::encoderInputFormatFor(Codec::H264, EncoderParams{}) == "NV12");

    pretendInstalled({"vah264dec", "avdec_h264", "x264enc"});
    VS_CHECK(media::encoderInputFormatFor(Codec::H264, EncoderParams{}) == "I420");
    restoreRealProbe();
}

VS_TEST(resolving_reports_nothing_when_no_provider_offers_the_role) {
    pretendInstalled({"avdec_h264", "x264enc"});  // no jpegenc
    VS_CHECK(!media::resolveJpegEncoder(85).has_value());
    // H265 encode: software declines it, and nothing else is installed.
    VS_CHECK(!media::resolveEncoder(Codec::H265, EncoderParams{}).has_value());
    restoreRealProbe();
}

// A provider offers three INDEPENDENT roles, and a machine routinely has the
// elements for some and not others. Measured on an RK3588 board: jpegenc and
// avdec_h264 present, x264enc absent — and because the software provider gated
// every role on x264enc, the board lost JPEG entirely and both snapshots and
// thumbnails answered 503 on a machine that could encode one perfectly well.

VS_TEST(a_missing_encoder_does_not_take_the_jpeg_encoder_with_it) {
    // Exactly the board: libav and jpegenc, no x264enc.
    media::setElementProbe([](const std::string& factory) {
        return factory == "avdec_h264" || factory == "avdec_h265" || factory == "jpegenc";
    });

    VS_CHECK(media::resolveJpegEncoder(85).has_value());
    VS_CHECK(media::resolveDecoder(Codec::H264).has_value());
    // And it is still honest about the encoder it does not have.
    VS_CHECK(!media::resolveEncoder(Codec::H264, EncoderParams{}).has_value());

    media::setElementProbe(nullptr);
}

VS_TEST(a_provider_never_resolves_to_an_element_that_is_not_installed) {
    // The general form of the bug above: availability is one boolean per
    // provider, so a role must be checked on its own before it is handed out.
    media::setElementProbe([](const std::string& factory) { return factory == "jpegenc"; });

    VS_CHECK(!media::resolveDecoder(Codec::H264).has_value());
    VS_CHECK(!media::resolveDecoder(Codec::H265).has_value());
    VS_CHECK(!media::resolveEncoder(Codec::H264, EncoderParams{}).has_value());
    const auto jpeg = media::resolveJpegEncoder(85);
    VS_CHECK(jpeg.has_value());
    if (jpeg) VS_CHECK(jpeg->spec.factory() == "jpegenc");

    media::setElementProbe(nullptr);
}

VS_MAIN()
