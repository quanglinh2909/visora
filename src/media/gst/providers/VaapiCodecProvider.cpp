// Intel and AMD via VA-API, using the modern "va" elements from
// gst-plugins-bad rather than the older gstreamer-vaapi ones.
//
// The va elements register only when a usable render node is present, so
// elementExists() answers the real question — is there a GPU here that will
// take this work — rather than whether a package happens to be installed.

#include <memory>

#include "media/gst/CodecProvider.hpp"
#include "media/gst/ElementAvailability.hpp"

namespace visora::media {
namespace {

// The predecessor found vah264dec installed with rank=none on its test machine,
// so decodebin autoplugged software decode even with a GPU present. Naming the
// element explicitly, as this provider does, is what makes the GPU get used.
class VaapiCodecProvider final : public CodecProvider {
public:
    std::string_view id() const override { return "vaapi"; }

    bool available() const override {
        return elementExists("vah264dec") || elementExists("vah264enc");
    }

    std::optional<ElementSpec> decoder(Codec codec) const override {
        const char* factory = nullptr;
        switch (codec) {
            case Codec::H264: factory = "vah264dec"; break;
            case Codec::H265: factory = "vah265dec"; break;
            case Codec::Unknown: return std::nullopt;
        }
        if (!elementExists(factory)) return std::nullopt;
        return ElementSpec(factory);
    }

    std::optional<ElementSpec> encoder(Codec codec,
                                       const EncoderParams& params) const override {
        const char* factory = nullptr;
        switch (codec) {
            case Codec::H264: factory = "vah264enc"; break;
            case Codec::H265: factory = "vah265enc"; break;
            case Codec::Unknown: return std::nullopt;
        }
        if (!elementExists(factory)) return std::nullopt;

        ElementSpec spec(factory);
        // vah264enc takes kbit/s.
        if (params.bitrateKbps > 0) spec.set("bitrate", params.bitrateKbps);
        if (params.gopSize >= 0) spec.set("key-int-max", params.gopSize);
        if (params.lowLatency) spec.set("rate-control", "cbr");
        return spec;
    }

    std::optional<ElementSpec> jpegEncoder(int /*quality*/) const override {
        return std::nullopt;
    }

    std::string encoderInputFormat() const override { return "NV12"; }
    bool zeroCopyFrames() const override { return true; }
};

core::Probe probeVaapi() {
    const bool decode = elementExists("vah264dec");
    const bool encode = elementExists("vah264enc");
    if (!decode && !encode) {
        return core::Probe::no("va elements not registered (no VA-API render node, "
                               "or gstreamer1.0-plugins-bad built without it)");
    }
    std::string detail = "VA-API (";
    detail += decode ? "decode" : "no decode";
    detail += encode ? ", encode)" : ", no encode)";
    return core::Probe::yes(detail);
}

const core::Register<CodecProvider> registration{{
    "vaapi", 60, &probeVaapi,
    [] { return std::unique_ptr<CodecProvider>(new VaapiCodecProvider()); },
}};

}  // namespace
}  // namespace visora::media
