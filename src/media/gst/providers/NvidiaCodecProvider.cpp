// NVIDIA NVDEC / NVENC, via the gst-plugins-bad nvcodec plugin.
//
// Covers both Jetson and discrete GPUs: same element names, and the plugin only
// registers them when a usable device is present, so elementExists() is a real
// availability check rather than a guess about the hardware.

#include <memory>

#include "media/gst/CodecProvider.hpp"
#include "media/gst/ElementAvailability.hpp"

namespace visora::media {
namespace {

class NvidiaCodecProvider final : public CodecProvider {
public:
    std::string_view id() const override { return "nvidia"; }

    bool available() const override {
        return elementExists("nvh264dec") || elementExists("nvh264enc");
    }

    std::optional<ElementSpec> decoder(Codec codec) const override {
        const char* factory = nullptr;
        switch (codec) {
            case Codec::H264: factory = "nvh264dec"; break;
            case Codec::H265: factory = "nvh265dec"; break;
            case Codec::Unknown: return std::nullopt;
        }
        if (!elementExists(factory)) return std::nullopt;
        return ElementSpec(factory);
    }

    std::optional<ElementSpec> encoder(Codec codec,
                                       const EncoderParams& params) const override {
        const char* factory = nullptr;
        switch (codec) {
            case Codec::H264: factory = "nvh264enc"; break;
            case Codec::H265: factory = "nvh265enc"; break;
            case Codec::Unknown: return std::nullopt;
        }
        if (!elementExists(factory)) return std::nullopt;

        ElementSpec spec(factory);
        if (params.lowLatency) {
            // NVENC's own name for the latency-tuned rate control preset.
            spec.set("preset", "low-latency-hq").set("rc-mode", "cbr-ld-hq");
        }
        // nvh264enc takes kbit/s.
        if (params.bitrateKbps > 0) spec.set("bitrate", params.bitrateKbps);
        if (params.gopSize >= 0) spec.set("gop-size", params.gopSize);
        return spec;
    }

    std::optional<ElementSpec> jpegEncoder(int quality) const override {
        // nvjpegenc exposes no quality property in the versions seen in the
        // field; a caller asking for a specific quality would silently not get
        // it, so software JPEG handles this instead.
        (void)quality;
        return std::nullopt;
    }

    std::string encoderInputFormat() const override { return "NV12"; }
    bool zeroCopyFrames() const override { return true; }
};

core::Probe probeNvidia() {
    const bool decode = elementExists("nvh264dec");
    const bool encode = elementExists("nvh264enc");
    if (!decode && !encode) {
        return core::Probe::no("nvcodec elements not registered (no NVIDIA device, "
                               "or gstreamer1.0-plugins-bad built without it)");
    }
    std::string detail = "NVIDIA nvcodec (";
    detail += decode ? "decode" : "no decode";
    detail += encode ? ", encode)" : ", no encode)";
    return core::Probe::yes(detail);
}

const core::Register<CodecProvider> registration{{
    "nvidia", 80, &probeNvidia,
    [] { return std::unique_ptr<CodecProvider>(new NvidiaCodecProvider()); },
}};

}  // namespace
}  // namespace visora::media
