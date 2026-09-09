#include "vision/ResultSink.hpp"

#include "core/Log.hpp"

namespace visora::vision {
namespace {
constexpr const char* kCategory = "vision";
}

core::Registry<ResultSink>& resultSinkRegistry() {
    return core::Registry<ResultSink>::instance();
}

std::size_t ResultSinkSet::startAll() {
    m_sinks = resultSinkRegistry().selectAll("result-sink");

    // A sink that fails to start is dropped rather than propagated. Losing an
    // output is bad; refusing to run the cameras because an output is
    // unavailable is worse.
    for (auto it = m_sinks.begin(); it != m_sinks.end();) {
        const core::Status started = (*it)->start();
        if (started.ok()) {
            VS_INFO(kCategory) << "result sink '" << (*it)->id() << "' started";
            ++it;
        } else {
            VS_WARN(kCategory) << "result sink '" << (*it)->id()
                               << "' did not start: " << started.error().str()
                               << "; continuing without it";
            it = m_sinks.erase(it);
        }
    }
    return m_sinks.size();
}

void ResultSinkSet::stopAll() {
    for (auto& sink : m_sinks) sink->stop();
    m_sinks.clear();
}

void ResultSinkSet::publish(const Result& result) {
    for (auto& sink : m_sinks) sink->publish(result);
}

bool ResultSinkSet::anyConsumers() const {
    for (const auto& sink : m_sinks) {
        if (sink->hasConsumers()) return true;
    }
    return false;
}

std::vector<std::string> ResultSinkSet::activeIds() const {
    std::vector<std::string> ids;
    ids.reserve(m_sinks.size());
    for (const auto& sink : m_sinks) ids.emplace_back(sink->id());
    return ids;
}

}  // namespace visora::vision
