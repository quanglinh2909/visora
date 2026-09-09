#include "hal/ImageOps.hpp"

#include <cstdlib>
#include <mutex>

namespace visora::hal {

Registry<ImageOps>& imageOpsRegistry() { return Registry<ImageOps>::instance(); }

core::Result<ImageOps*> imageOps() {
    static std::mutex mutex;
    static std::unique_ptr<ImageOps> cached;
    static std::string failure;

    std::lock_guard<std::mutex> lock(mutex);
    if (cached) return cached.get();
    if (!failure.empty()) return core::notFound(failure);

    const char* forced = std::getenv("VISORA_IMAGE_BACKEND");
    auto selected = imageOpsRegistry().select("image-ops", forced ? forced : "");
    if (!selected) {
        failure = selected.error().message;
        return core::notFound(failure);
    }
    cached = std::move(selected.value());
    return cached.get();
}

}  // namespace visora::hal
