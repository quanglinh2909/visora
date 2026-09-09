#include "media/gst/CodecProvider.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <set>

#include "core/Log.hpp"
#include "media/gst/ElementAvailability.hpp"

namespace visora::media {
namespace {
constexpr const char* kCategory = "codec";
}

core::Registry<CodecProvider>& codecProviderRegistry() {
    return core::Registry<CodecProvider>::instance();
}

core::Result<CodecProvider*> codecProvider() {
    static std::mutex mutex;
    static std::unique_ptr<CodecProvider> cached;
    static std::string failure;
    static unsigned long cachedGeneration = 0;

    std::lock_guard<std::mutex> lock(mutex);
    // The selection is derived from what elements exist; if that answer can now
    // differ, the cached choice describes a machine we are no longer on.
    const unsigned long generation = elementProbeGeneration();
    if (generation != cachedGeneration) {
        cached.reset();
        failure.clear();
        cachedGeneration = generation;
    }
    if (cached) return cached.get();
    if (!failure.empty()) return core::notFound(failure);

    const char* forced = std::getenv("VISORA_CODEC_PROVIDER");
    auto selected = codecProviderRegistry().select("codec", forced ? forced : "");
    if (!selected) {
        failure = selected.error().message;
        return core::notFound(failure);
    }
    cached = std::move(selected.value());
    VS_INFO(kCategory) << "using codec provider '" << cached->id() << "'";
    return cached.get();
}

std::vector<CodecProvider*> availableCodecProviders() {
    // Owned for the process lifetime. Providers are stateless element-name
    // tables; keeping them alive is cheaper than rebuilding on every pipeline.
    static std::mutex mutex;
    static std::vector<std::unique_ptr<CodecProvider>> owned;
    static unsigned long cachedGeneration = 0;

    std::lock_guard<std::mutex> lock(mutex);
    const unsigned long generation = elementProbeGeneration();
    if (generation != cachedGeneration) {
        owned = codecProviderRegistry().selectAll("codec");
        cachedGeneration = generation;
    }
    std::vector<CodecProvider*> out;
    out.reserve(owned.size());
    for (const auto& provider : owned) out.push_back(provider.get());
    return out;
}

namespace {

// Logged once per role so a machine quietly doing software encode is visible in
// the log without drowning it at frame rate.
void noteOnce(const char* role, const std::string& providerId, const char* what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    const std::string key = std::string(role) + '|' + providerId + '|' + what;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!seen.insert(key).second) return;
    }
    VS_INFO(kCategory) << role << " for " << what << ": '" << providerId << "'";
}

}  // namespace

std::optional<ResolvedElement> resolveDecoder(Codec codec) {
    for (CodecProvider* provider : availableCodecProviders()) {
        if (auto spec = provider->decoder(codec)) {
            noteOnce("decoder", std::string(provider->id()), toString(codec));
            return ResolvedElement{std::move(*spec), std::string(provider->id())};
        }
    }
    return std::nullopt;
}

std::optional<ResolvedElement> resolveEncoder(Codec codec, const EncoderParams& params) {
    for (CodecProvider* provider : availableCodecProviders()) {
        if (auto spec = provider->encoder(codec, params)) {
            noteOnce("encoder", std::string(provider->id()), toString(codec));
            return ResolvedElement{std::move(*spec), std::string(provider->id())};
        }
    }
    return std::nullopt;
}

std::optional<ResolvedElement> resolveJpegEncoder(int quality) {
    for (CodecProvider* provider : availableCodecProviders()) {
        if (auto spec = provider->jpegEncoder(quality)) {
            noteOnce("jpeg encoder", std::string(provider->id()), "jpeg");
            return ResolvedElement{std::move(*spec), std::string(provider->id())};
        }
    }
    return std::nullopt;
}

std::string encoderInputFormatFor(Codec codec, const EncoderParams& params) {
    for (CodecProvider* provider : availableCodecProviders()) {
        if (provider->encoder(codec, params)) return provider->encoderInputFormat();
    }
    return "I420";
}

std::vector<core::BackendStatus> codecBackendStatus() {
    std::string selected;
    auto chosen = codecProvider();
    if (chosen) selected = std::string(chosen.value()->id());
    return codecProviderRegistry().status("codec", selected);
}

}  // namespace visora::media
