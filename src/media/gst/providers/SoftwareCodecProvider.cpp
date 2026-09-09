// Software codecs. Always last, always there.
//
// Priority 0: any hardware provider that finds its elements wins. This one
// exists so a machine with no video hardware still runs the whole product,
// slower — the same role CpuImageOps plays for pixels.

#include <memory>
#include <string>

#include "media/gst/CodecProvider.hpp"
#include "media/gst/ElementAvailability.hpp"

namespace visora::media {
namespace {

class SoftwareCodecProvider final : public CodecProvider {
public:
    std::string_view id() const override { return "software"; }

    bool available() const override {
        // ANY of the three roles, not all of them.
        //
        // libav decoders ship with gstreamer1.0-libav, x264enc with -ugly, and
        // jpegenc with -good; a machine routinely has some and not others.
        // Requiring all three cost an RK3588 board its JPEG encoder — it has
        // jpegenc and avdec_h264 but no x264enc, so the whole provider was
        // dropped and snapshots and thumbnails answered 503 on a machine that
        // could encode a JPEG perfectly well.
        //
        // Which roles actually work is decided per role, in resolveDecoder /
        // resolveEncoder / resolveJpegEncoder, which check that the element a
        // provider names is installed.
        return elementExists("avdec_h264") || elementExists("x264enc") ||
               elementExists("jpegenc");
    }

    std::optional<ElementSpec> decoder(Codec codec) const override {
        switch (codec) {
            case Codec::H264: return ElementSpec("avdec_h264");
            case Codec::H265: return ElementSpec("avdec_h265");
            case Codec::Unknown: break;
        }
        return std::nullopt;
    }


    std::optional<ElementSpec> encoder(Codec codec,
                                       const EncoderParams& params) const override {
        // Only H264 is offered. x265enc is rarely installed, and every consumer
        // of a transcode here is a browser, which does not accept H265 anyway.
        if (codec != Codec::H264) return std::nullopt;

        ElementSpec spec("x264enc");
        if (params.lowLatency) {
            spec.set("tune", "zerolatency").set("speed-preset", "ultrafast");
        }
        // x264enc takes kbit/s directly, unlike the hardware encoders which
        // want bits per second.
        if (params.bitrateKbps > 0) spec.set("bitrate", params.bitrateKbps);
        if (params.gopSize >= 0) spec.set("key-int-max", params.gopSize);
        return spec;
    }

    std::optional<ElementSpec> jpegEncoder(int quality) const override {
        if (!elementExists("jpegenc")) return std::nullopt;
        return ElementSpec("jpegenc").set("quality", quality);
    }

    std::string encoderInputFormat() const override { return "I420"; }
};

core::Probe probeSoftware() {
    // Available when ANY role works, and the description says which.
    //
    // These three elements come from three different packages — libav, -ugly
    // and -good — and a machine routinely has some and not others. Requiring
    // all of them cost an RK3588 board its JPEG encoder: it has jpegenc and
    // avdec_h264 but no x264enc, so the provider was excluded entirely and both
    // snapshots and thumbnails answered 503 on a machine that could encode a
    // JPEG perfectly well. Which roles actually work is then decided per role,
    // in resolveDecoder / resolveEncoder / resolveJpegEncoder.
    const bool decode = elementExists("avdec_h264");
    const bool encode = elementExists("x264enc");
    const bool jpeg = elementExists("jpegenc");
    if (!decode && !encode && !jpeg) {
        // Names the elements, not just the packages: an operator reading this
        // on a board needs to know what to look for with gst-inspect.
        return core::Probe::no("none of avdec_h264, x264enc, jpegenc installed "
                               "(gstreamer1.0-libav, -plugins-ugly, -plugins-good)");
    }

    std::string roles;
    const auto add = [&roles](const char* role) {
        if (!roles.empty()) roles += ", ";
        roles += role;
    };
    if (decode) add("libav decode");
    if (encode) add("x264 encode");
    if (jpeg) add("jpeg");
    return core::Probe::yes("software codecs (" + roles + ')');
}

const core::Register<CodecProvider> registration{{
    "software", 0, &probeSoftware,
    [] { return std::unique_ptr<CodecProvider>(new SoftwareCodecProvider()); },
}};

}  // namespace
}  // namespace visora::media
