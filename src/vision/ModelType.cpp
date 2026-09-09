#include "vision/ModelType.hpp"

#include <algorithm>

namespace visora::vision {

const char* toString(FramePrep prep) {
    switch (prep) {
        case FramePrep::Letterbox: return "letterbox";
        case FramePrep::Stretch:   return "stretch";
        case FramePrep::FitHeight: return "fit-height";
    }
    return "letterbox";
}

core::Status ModelType::enrich(const hal::TensorSet& tensors, const ModelContext& context,
                               Detection& parent) const {
    auto children = decode(tensors, context);
    if (!children) return children.error();

    for (Detection& child : children.value()) {
        // The label comes from THIS type, because the child was produced by it.
        // An OCR dictionary lives with the model that used it.
        if (child.text.empty()) child.text = labelFor(child.classId);
        parent.children.push_back(std::move(child));
    }
    return {};
}

core::Registry<ModelType>& modelTypeRegistry() {
    return core::Registry<ModelType>::instance();
}

namespace {

// Built once and shared. Model types are stateless — they turn tensors into
// detections and hold nothing — so one instance serves every job worker, and
// handing out raw pointers to instances that live for the life of the process
// keeps the callers free of ownership questions.
//
// Rebuilt when the registry grows, which only happens in a test that registers
// a type of its own.
std::vector<std::unique_ptr<ModelType>>& instances() {
    static std::vector<std::unique_ptr<ModelType>> built;
    static std::size_t knownCount = 0;
    const std::size_t registered = modelTypeRegistry().size();
    if (built.empty() || registered != knownCount) {
        built = modelTypeRegistry().selectAll("model");
        knownCount = registered;
    }
    return built;
}

}  // namespace

std::vector<ModelType*> modelTypes() {
    std::vector<ModelType*> out;
    for (const auto& type : instances()) out.push_back(type.get());
    return out;
}

ModelType* modelType(std::string_view id) {
    for (const auto& type : instances()) {
        if (type->id() == id) return type.get();
    }
    return nullptr;
}

}  // namespace visora::vision
