#include "media/gst/ElementSpec.hpp"

#include <algorithm>
#include <cstdio>

namespace visora::media {
namespace {

// Renders a double without a trailing ".000000", which GStreamer accepts but
// which makes golden strings noisy.
std::string renderDouble(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

}  // namespace

std::string quoteLaunchValue(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (const char c : value) {
        if (c == '"' || c == '\\') quoted.push_back('\\');
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

ElementSpec& ElementSpec::named(std::string instanceName) {
    m_instanceName = std::move(instanceName);
    return *this;
}

ElementSpec& ElementSpec::setRaw(std::string property, std::string rendered) {
    for (auto& entry : m_properties) {
        if (entry.first == property) {
            entry.second = std::move(rendered);
            return *this;
        }
    }
    m_properties.emplace_back(std::move(property), std::move(rendered));
    return *this;
}

ElementSpec& ElementSpec::set(std::string property, std::string value) {
    return setRaw(std::move(property), std::move(value));
}

ElementSpec& ElementSpec::set(std::string property, const char* value) {
    return setRaw(std::move(property), value == nullptr ? std::string() : std::string(value));
}

ElementSpec& ElementSpec::set(std::string property, int value) {
    return setRaw(std::move(property), std::to_string(value));
}

ElementSpec& ElementSpec::set(std::string property, unsigned value) {
    return setRaw(std::move(property), std::to_string(value));
}

ElementSpec& ElementSpec::set(std::string property, long long value) {
    return setRaw(std::move(property), std::to_string(value));
}

ElementSpec& ElementSpec::set(std::string property, double value) {
    return setRaw(std::move(property), renderDouble(value));
}

ElementSpec& ElementSpec::set(std::string property, bool value) {
    return setRaw(std::move(property), value ? "true" : "false");
}

ElementSpec& ElementSpec::setQuoted(std::string property, const std::string& value) {
    return setRaw(std::move(property), quoteLaunchValue(value));
}

bool ElementSpec::has(const std::string& property) const {
    return std::any_of(m_properties.begin(), m_properties.end(),
                       [&](const auto& entry) { return entry.first == property; });
}

std::string ElementSpec::valueOf(const std::string& property) const {
    for (const auto& entry : m_properties) {
        if (entry.first == property) return entry.second;
    }
    return {};
}

std::string ElementSpec::toLaunch() const {
    if (!valid()) return {};

    std::string out = m_factory;
    // The name comes first so a reader can tell at a glance which element a
    // long property list belongs to.
    if (!m_instanceName.empty()) {
        out += " name=";
        out += m_instanceName;
    }
    for (const auto& [property, value] : m_properties) {
        out += ' ';
        out += property;
        out += '=';
        out += value;
    }
    return out;
}

}  // namespace visora::media
