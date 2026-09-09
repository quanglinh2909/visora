// Kernel V4L2 memory-to-memory codecs: Raspberry Pi, Amlogic, NXP and most
// other ARM boards whose vendor exposes hardware video through V4L2 rather than
// a proprietary library.
//
// Lower priority than the vendor-specific providers because a board with both
// usually gets better throughput from its own stack, but far above software.

#include <memory>

#include "media/gst/CodecProvider.hpp"
#include "media/gst/ElementAvailability.hpp"

namespace visora::media {
namespace {

class V4l2CodecProvider final : public CodecProvider {
public:
    std::string_view id() const override { return "v4l2"; }

    bool available() const override {
        // v4l2 elements are registered per device found at plugin scan time, so
        // their presence really does mean the kernel exposed a codec device.
        return elementExists("v4l2h264dec") || elementExists("v4l2h264enc");
    }

    std::optional<ElementSpec> decoder(Codec codec) const override {
        const char* factory = nullptr;
        switch (codec) {
            case Codec::H264: factory = "v4l2h264dec"; break;
            case Codec::H265: factory = "v4l2h265dec"; break;
            case Codec::Unknown: return std::nullopt;
        }
        if (!elementExists(factory)) return std::nullopt;
        return ElementSpec(factory);
    }

    std::optional<ElementSpec> encoder(Codec codec,
                                       const EncoderParams& params) const override {
        if (codec != Codec::H264) return std::nullopt;
        if (!elementExists("v4l2h264enc")) return std::nullopt;

        ElementSpec spec("v4l2h264enc");
        // V4L2 encoders take their settings through a controls structure rather
        // than as plain properties, and the accepted control names vary by
        // driver. Bitrate is the one that is consistently spelled this way.
        if (params.bitrateKbps > 0) {
            spec.set("extra-controls",
                     "controls,video_bitrate=" +
                         std::to_string(static_cast<long long>(params.bitrateKbps) * 1000LL));
        }
        return spec;
    }

    std::optional<ElementSpec> jpegEncoder(int /*quality*/) const override {
        return std::nullopt;
    }

    std::string encoderInputFormat() const override { return "NV12"; }
};

core::Probe probeV4l2() {
    const bool decode = elementExists("v4l2h264dec");
    const bool encode = elementExists("v4l2h264enc");
    if (!decode && !encode) {
        return core::Probe::no("no V4L2 codec device exposed by the kernel");
    }
    std::string detail = "V4L2 m2m (";
    detail += decode ? "decode" : "no decode";
    detail += encode ? ", encode)" : ", no encode)";
    return core::Probe::yes(detail);
}

const core::Register<CodecProvider> registration{{
    "v4l2", 40, &probeV4l2,
    [] { return std::unique_ptr<CodecProvider>(new V4l2CodecProvider()); },
}};

}  // namespace
}  // namespace visora::media
