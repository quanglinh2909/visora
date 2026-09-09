#pragma once

// Which elements this machine should use to decode and encode video.
//
// The three things that genuinely differ between machines. Everything else
// about a pipeline — depayloaders, parsers, payloaders, queues, tees — is the
// same everywhere and lives in Codec.hpp.
//
// Adding hardware is a new provider file plus a registration; no pipeline
// changes anywhere. That is the whole point, and codec_tests asserts it by
// registering a provider from the test file alone.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Registry.hpp"
#include "core/Result.hpp"
#include "media/gst/Codec.hpp"
#include "media/gst/ElementSpec.hpp"

namespace visora::media {

struct EncoderParams {
    // 0 means "let the encoder decide". Worth setting: MPP's own estimate for
    // 1080p works out around 6.5 Mbps, several times what the source stream
    // actually uses.
    int bitrateKbps = 0;

    // -1 follows the input's own keyframes rather than imposing a GOP, which is
    // what a transcode of an existing stream wants.
    int gopSize = -1;

    // Tune for latency over compression. True for live restream, false when
    // writing a file nobody is watching in real time.
    bool lowLatency = true;
};

class CodecProvider {
public:
    virtual ~CodecProvider() = default;

    virtual std::string_view id() const = 0;

    // Whether the elements this provider names are actually installed. Checked
    // through elementExists() so it can be exercised in tests.
    virtual bool available() const = 0;

    virtual std::optional<ElementSpec> decoder(Codec codec) const = 0;
    virtual std::optional<ElementSpec> encoder(Codec codec,
                                               const EncoderParams& params) const = 0;
    virtual std::optional<ElementSpec> jpegEncoder(int quality) const = 0;

    // The raw video format the encoder wants fed to it. Feeding an MPP encoder
    // I420 makes it convert on the CPU, silently undoing the point of using it.
    virtual std::string encoderInputFormat() const { return "I420"; }

    // Whether decoded frames arrive as dmabuf that the image layer can import
    // without a copy.
    virtual bool zeroCopyFrames() const { return false; }
};

core::Registry<CodecProvider>& codecProviderRegistry();

// The best available provider, cached. `VISORA_CODEC_PROVIDER` forces one,
// which is how you compare hardware against software on the same machine.
core::Result<CodecProvider*> codecProvider();

// Every available provider, best first. Callers that need a specific
// capability — a JPEG encoder, say — walk this rather than giving up when the
// best provider happens not to offer that one thing.
std::vector<CodecProvider*> availableCodecProviders();

// An element, and which provider supplied it.
struct ResolvedElement {
    ElementSpec spec;
    std::string providerId;
};

// Resolve a role across ALL available providers, best first.
//
// Prefer these to codecProvider(): the best provider does not necessarily offer
// every role. A machine can have VA-API decode with no VA-API encoder, and
// Rockchip deliberately offers no JPEG encoder at all — asking the selected
// provider alone would fail on a machine that can do the work perfectly well
// with the next one down.
std::optional<ResolvedElement> resolveDecoder(Codec codec);
std::optional<ResolvedElement> resolveEncoder(Codec codec, const EncoderParams& params);
std::optional<ResolvedElement> resolveJpegEncoder(int quality);

// The raw format the provider that would supply the encoder wants. Kept beside
// the resolution so a caller cannot pair one provider's encoder with another's
// input format.
std::string encoderInputFormatFor(Codec codec, const EncoderParams& params);

// Codec providers as capability-report rows.
//
// Returned rather than pushed into hal::Capabilities: media sits above hal in
// the dependency order, so hal must not know this layer exists. Whoever
// assembles the report — an application, not a library — joins the two.
std::vector<core::BackendStatus> codecBackendStatus();

}  // namespace visora::media
