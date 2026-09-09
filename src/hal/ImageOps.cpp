#include "hal/ImageOps.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "core/Log.hpp"

namespace visora::hal {
namespace {

constexpr const char* kCategory = "hal";

// Whether a failure means "ask the next backend" rather than "the caller got it
// wrong". An accelerator refusing a scale ratio or a format is routine and the
// software path should pick it up; a caller passing a degenerate rectangle is
// not, and must surface rather than be silently retried.
bool shouldFallThrough(const core::Error& error) {
    return error.code == core::ErrorCode::Unsupported ||
           error.code == core::ErrorCode::HardwareFailure;
}

// Tries each backend in turn, best first.
//
// This is composition rather than inheritance on purpose: a backend stays a
// simple thing that either performs an operation or says it cannot, and the
// policy for what to do about that lives in exactly one place instead of being
// duplicated as an ad-hoc software fallback inside every accelerated path.
class ChainedImageOps final : public ImageOps {
public:
    explicit ChainedImageOps(std::vector<std::unique_ptr<ImageOps>> chain)
        : m_chain(std::move(chain)) {
        m_id = "chain";
        for (const auto& ops : m_chain) {
            m_id += m_id == "chain" ? ":" : "+";
            m_id += std::string(ops->id());
        }
    }

    std::string_view id() const override { return m_id; }

    bool supportsZeroCopy() const override {
        return !m_chain.empty() && m_chain.front()->supportsZeroCopy();
    }

    core::Result<core::Rect> fit(const core::ImageView& src,
                                 const core::MutableImageView& dst,
                                 core::FitMode mode, std::uint8_t padValue) override {
        core::Error last = core::notFound("no image-ops backend available");
        for (const auto& ops : m_chain) {
            auto result = ops->fit(src, dst, mode, padValue);
            if (result.ok()) return result;
            if (!shouldFallThrough(result.error())) return result;
            noteFallback("fit", ops->id(), result.error());
            last = result.error();
        }
        return last;
    }

    core::Status crop(const core::ImageView& src, core::Rect roi,
                      const core::MutableImageView& dst) override {
        core::Error last = core::notFound("no image-ops backend available");
        for (const auto& ops : m_chain) {
            const core::Status result = ops->crop(src, roi, dst);
            if (result.ok()) return result;
            if (!shouldFallThrough(result.error())) return result;
            noteFallback("crop", ops->id(), result.error());
            last = result.error();
        }
        return last;
    }

    core::Status convert(const core::ImageView& src,
                         const core::MutableImageView& dst) override {
        core::Error last = core::notFound("no image-ops backend available");
        for (const auto& ops : m_chain) {
            const core::Status result = ops->convert(src, dst);
            if (result.ok()) return result;
            if (!shouldFallThrough(result.error())) return result;
            noteFallback("convert", ops->id(), result.error());
            last = result.error();
        }
        return last;
    }

    // Zero-copy import is not a fallback chain: only the front backend can
    // hold a meaningful reference, and a handle from one backend means nothing
    // to another.
    core::Result<NativeHandle> import(const core::ImageView& image) override {
        if (m_chain.empty()) return core::notFound("no image-ops backend available");
        return m_chain.front()->import(image);
    }

private:
    // Falling back is normal but worth knowing about — it is the difference
    // between "the NPU box is fast" and "the NPU box is quietly doing
    // everything in software". Logged once per (op, backend, reason) so a
    // per-frame fallback does not drown the log.
    void noteFallback(const char* op, std::string_view from, const core::Error& why) {
        const std::string key = std::string(op) + '|' + std::string(from) + '|' + why.message;
        {
            std::lock_guard<std::mutex> lock(m_seenMutex);
            if (!m_seen.insert(key).second) return;
        }
        VS_WARN(kCategory) << "image-ops: '" << from << "' cannot " << op << " ("
                           << why.str() << "); falling back to software. "
                           << "This message appears once per distinct reason.";
    }

    std::vector<std::unique_ptr<ImageOps>> m_chain;
    std::string m_id;
    std::mutex m_seenMutex;
    std::unordered_set<std::string> m_seen;
};

}  // namespace

Registry<ImageOps>& imageOpsRegistry() { return Registry<ImageOps>::instance(); }

core::Result<ImageOps*> imageOps() {
    static std::mutex mutex;
    static std::unique_ptr<ImageOps> cached;
    static std::string failure;

    std::lock_guard<std::mutex> lock(mutex);
    if (cached) return cached.get();
    if (!failure.empty()) return core::notFound(failure);

    // Forcing one backend disables the chain deliberately: that is how you find
    // out whether the accelerator really handles a case, rather than measuring
    // the software path wearing its name.
    if (const char* forced = std::getenv("VISORA_IMAGE_BACKEND")) {
        auto selected = imageOpsRegistry().select("image-ops", forced);
        if (!selected) {
            failure = selected.error().message;
            return core::notFound(failure);
        }
        cached = std::move(selected.value());
        return cached.get();
    }

    auto available = imageOpsRegistry().selectAll("image-ops");
    if (available.empty()) {
        failure = "no image-ops backend is available";
        return core::notFound(failure);
    }
    if (available.size() == 1) {
        cached = std::move(available.front());
        VS_INFO(kCategory) << "image-ops: using '" << cached->id() << "' (no fallback needed)";
        return cached.get();
    }

    cached = std::unique_ptr<ImageOps>(new ChainedImageOps(std::move(available)));
    VS_INFO(kCategory) << "image-ops: using " << cached->id();
    return cached.get();
}

}  // namespace visora::hal
