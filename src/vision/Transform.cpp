#include "vision/Transform.hpp"

namespace visora::vision {

core::Registry<Transform>& transformRegistry() { return core::Registry<Transform>::instance(); }

namespace {

// Stateless singletons shared by every job worker thread — see the header.
std::vector<std::unique_ptr<Transform>>& instances() {
    static std::vector<std::unique_ptr<Transform>> built;
    static std::size_t knownCount = 0;
    const std::size_t registered = transformRegistry().size();
    if (built.empty() || registered != knownCount) {
        built = transformRegistry().selectAll("transform");
        knownCount = registered;
    }
    return built;
}

}  // namespace

std::vector<Transform*> transforms() {
    std::vector<Transform*> out;
    for (const auto& item : instances()) out.push_back(item.get());
    return out;
}

// The transform a stage names, with the empty id meaning "the default".
//
// The crop registers under its own name so a client can see it in
// GET /ai-transforms and refer to it — it used to register as "", which listed
// an entry no caller could name. An empty id in a stage still resolves here, so
// a job that omits the field, and every job stored before this, keeps working.
Transform* transform(std::string_view id) {
    const std::string_view wanted = id.empty() ? kDefaultTransformId : id;
    for (const auto& item : instances()) {
        if (item->id() == wanted) return item.get();
    }
    return nullptr;
}

}  // namespace visora::vision
