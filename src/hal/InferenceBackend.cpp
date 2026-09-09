#include "hal/InferenceBackend.hpp"

#include <cstdlib>
#include <functional>
#include <mutex>
#include <numeric>

namespace visora::hal {

const char* toString(TensorType type) {
    switch (type) {
        case TensorType::Int8:    return "int8";
        case TensorType::UInt8:   return "uint8";
        case TensorType::Float32: return "float32";
        case TensorType::Unknown: return "unknown";
    }
    return "unknown";
}

std::size_t Tensor::elementCount() const {
    if (shape.empty()) return 0;
    return std::accumulate(shape.begin(), shape.end(), std::size_t{1},
                           [](std::size_t acc, int dim) {
                               return dim > 0 ? acc * static_cast<std::size_t>(dim) : acc;
                           });
}

const Tensor* TensorSet::find(std::string_view name) const {
    for (const Tensor& tensor : outputs) {
        if (tensor.name == name) return &tensor;
    }
    return nullptr;
}

Registry<InferenceBackend>& inferenceRegistry() {
    return Registry<InferenceBackend>::instance();
}

core::Result<InferenceBackend*> inference() {
    static std::mutex mutex;
    static std::unique_ptr<InferenceBackend> cached;
    static std::string failure;

    std::lock_guard<std::mutex> lock(mutex);
    if (cached) return cached.get();
    if (!failure.empty()) return core::notFound(failure);

    const char* forced = std::getenv("VISORA_INFERENCE_BACKEND");
    auto selected = inferenceRegistry().select("inference", forced ? forced : "");
    if (!selected) {
        failure = selected.error().message;
        return core::notFound(failure);
    }
    cached = std::move(selected.value());
    return cached.get();
}

}  // namespace visora::hal
