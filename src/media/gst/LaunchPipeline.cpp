#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {

LaunchChain& LaunchChain::add(const ElementSpec& element) {
    if (element.valid()) m_parts.push_back(element.toLaunch());
    return *this;
}

LaunchChain& LaunchChain::add(const std::string& factory) {
    if (!factory.empty()) m_parts.push_back(factory);
    return *this;
}

LaunchChain& LaunchChain::caps(const std::string& capsString) {
    if (!capsString.empty()) m_parts.push_back(capsString);
    return *this;
}

LaunchChain& LaunchChain::raw(const std::string& fragment) {
    if (!fragment.empty()) m_parts.push_back(fragment);
    return *this;
}

std::string LaunchChain::toLaunch() const {
    std::string out;
    for (std::size_t i = 0; i < m_parts.size(); ++i) {
        if (i != 0) out += " ! ";
        out += m_parts[i];
    }
    return out;
}

LaunchChain& LaunchPipeline::chain() {
    for (Branch& branch : m_branches) {
        if (branch.teeName.empty()) return branch.chain;
    }
    m_branches.push_back(Branch{});
    return m_branches.back().chain;
}

LaunchChain& LaunchPipeline::branch(const std::string& teeName) {
    m_branches.push_back(Branch{teeName, LaunchChain{}});
    return m_branches.back().chain;
}

std::string LaunchPipeline::toLaunch(bool wrapped) const {
    std::string out;
    for (const Branch& branch : m_branches) {
        if (branch.chain.empty()) continue;
        if (!out.empty()) out += ' ';
        if (!branch.teeName.empty()) {
            out += branch.teeName;
            out += ". ! ";
        }
        out += branch.chain.toLaunch();
    }
    if (wrapped) return "( " + out + " )";
    return out;
}

}  // namespace visora::media
