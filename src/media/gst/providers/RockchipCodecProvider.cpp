// Rockchip MPP codecs (RK35xx).
//
// No vendor SDK here — these are GStreamer element names, and the elements come
// from the board's own gstreamer1.0-rockchip package. That is why this lives in
// media rather than hal/rockchip: nothing includes a Rockchip header.

#include <memory>

#include "media/gst/CodecProvider.hpp"
#include "media/gst/ElementAvailability.hpp"

namespace visora::media {
namespace {

class RockchipCodecProvider final : public CodecProvider {
public:
    std::string_view id() const override { return "rockchip-mpp"; }

    bool available() const override { return elementExists("mppvideodec"); }

    std::optional<ElementSpec> decoder(Codec codec) const override {
        // One decoder for both codecs: mppvideodec takes whatever the parser in
        // front of it negotiated. On these boards it is often the ONLY decoder
        // installed, so leaving it out of a preference list means resolving to
        // no decoder at all rather than falling back to something slower.
        if (codec == Codec::Unknown) return std::nullopt;
        return ElementSpec("mppvideodec");
    }

    std::optional<ElementSpec> encoder(Codec codec,
                                       const EncoderParams& params) const override {
        const char* factory = nullptr;
        switch (codec) {
            case Codec::H264: factory = "mpph264enc"; break;
            case Codec::H265: factory = "mpph265enc"; break;
            case Codec::Unknown: return std::nullopt;
        }
        if (!elementExists(factory)) return std::nullopt;

        ElementSpec spec(factory);
        // gop=-1 follows the source's own keyframes instead of imposing a GOP,
        // which is what a transcode of an existing stream wants.
        spec.set("gop", params.gopSize);
        spec.set("rc-mode", params.lowLatency ? "vbr" : "cbr");
        // bps is bits per second here, not kbit/s. Left at 0 the encoder
        // estimates w*h*fps/8, which comes out around 6.5 Mbps for 1080p —
        // several times what the source stream actually uses.
        spec.set("bps", static_cast<long long>(params.bitrateKbps) * 1000LL);
        return spec;
    }

    std::optional<ElementSpec> jpegEncoder(int /*quality*/) const override {
        // Deliberately none.
        //
        // mppjpegenc produces green frames for some inputs — most reproducibly
        // NV12 out of mppvideodec on a 1080p H265 stream — and has been seen to
        // fault its jpege core hard enough to freeze the board. The predecessor
        // measured the same pipeline with jpegenc substituted and got correct
        // images every time. Software JPEG costs a few milliseconds on a
        // snapshot; the hardware path costs correctness.
        return std::nullopt;
    }

    // Feeding NV12 keeps the encoder from converting on the CPU, which would
    // undo most of the benefit of using it.
    std::string encoderInputFormat() const override { return "NV12"; }

    bool zeroCopyFrames() const override { return true; }
};

core::Probe probeRockchip() {
    if (!elementExists("mppvideodec")) {
        return core::Probe::no("mppvideodec not installed (gstreamer1.0-rockchip)");
    }
    const bool encoder = elementExists("mpph264enc");
    return core::Probe::yes(std::string("Rockchip MPP (decode") +
                            (encoder ? ", H264 encode)" : ", no encoder)"));
}

const core::Register<CodecProvider> registration{{
    "rockchip-mpp", 100, &probeRockchip,
    [] { return std::unique_ptr<CodecProvider>(new RockchipCodecProvider()); },
}};

}  // namespace
}  // namespace visora::media
