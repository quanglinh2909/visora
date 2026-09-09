// Software codecs. Always last, always there.
//
// Priority 0: any hardware provider that finds its elements wins. This one
// exists so a machine with no video hardware still runs the whole product,
// slower — the same role CpuImageOps plays for pixels.

#include <memory>

#include "media/gst/CodecProvider.hpp"
#include "media/gst/ElementAvailability.hpp"

namespace visora::media {
namespace {

class SoftwareCodecProvider final : public CodecProvider {
public:
    std::string_view id() const override { return "software"; }

    bool available() const override {
        // libav decoders ship with gstreamer1.0-libav; x264enc with -ugly. A
        // machine could have one and not the other, so this reports honestly
        // rather than assuming software is a given.
        return elementExists("avdec_h264") && elementExists("x264enc");
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
    if (!elementExists("avdec_h264")) {
        return core::Probe::no("avdec_h264 not installed (gstreamer1.0-libav)");
    }
    if (!elementExists("x264enc")) {
        return core::Probe::no("x264enc not installed (gstreamer1.0-plugins-ugly)");
    }
    return core::Probe::yes("software codecs (libav decode, x264 encode)");
}

const core::Register<CodecProvider> registration{{
    "software", 0, &probeSoftware,
    [] { return std::unique_ptr<CodecProvider>(new SoftwareCodecProvider()); },
}};

}  // namespace
}  // namespace visora::media
