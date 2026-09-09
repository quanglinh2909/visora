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

Transform* transform(std::string_view id) {
    for (const auto& item : instances()) {
        if (item->id() == id) return item.get();
    }
    return nullptr;
}

}  // namespace visora::vision
