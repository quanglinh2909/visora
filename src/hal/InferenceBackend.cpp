#include "hal/InferenceBackend.hpp"

#include <cstdlib>
#include <functional>
#include <mutex>
#include <vector>
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

std::vector<InferenceBackend*> availableInferenceBackends() {
    // Built once and kept. A backend holds a runtime handle — an NPU context,
    // an ONNX Runtime environment — that is expensive to create and meant to be
    // shared by every model loaded on it.
    static std::mutex mutex;
    static std::vector<std::unique_ptr<InferenceBackend>> cached;
    static bool built = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (!built) {
        cached = inferenceRegistry().selectAll("inference");
        built = true;
    }

    std::vector<InferenceBackend*> out;
    out.reserve(cached.size());
    for (const auto& backend : cached) out.push_back(backend.get());
    return out;
}

core::Result<std::unique_ptr<Model>> loadModel(const ModelRef& model) {
    const auto backends = availableInferenceBackends();
    if (backends.empty()) {
        return core::notFound("no inference backend is available on this machine");
    }

    std::string tried;
    for (InferenceBackend* backend : backends) {
        if (!backend->handles(model)) {
            if (!tried.empty()) tried += ", ";
            tried += backend->id();
            continue;
        }
        auto loaded = backend->load(model);
        if (loaded) {
            VS_INFO("hal") << "model " << model.path << " loaded on '" << backend->id() << '\'';
            return loaded;
        }
        // A backend that CLAIMED the artefact and then failed is reported as
        // it is: falling through to one that does not understand the format
        // would replace a real error with a confusing one.
        return loaded.error();
    }
    return core::unsupported("no backend handles " + model.path +
                             (tried.empty() ? "" : " (tried " + tried + ')'));
}

}  // namespace visora::hal
