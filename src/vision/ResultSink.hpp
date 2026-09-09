#pragma once

// Where inference results go.
//
// The predecessor hardcoded one destination: a Unix socket carrying one wire
// format. Adding an MQTT topic, a webhook or a database write meant editing the
// pipeline itself — the business output was welded to the machinery that
// produced it.
//
// A sink is a registered plug-in, exactly like a hardware backend. Several run
// at once, because "publish to the Python consumer AND record to the database"
// is an ordinary requirement, not a special case. This is the one place in the
// system where more than one implementation is used rather than the best one
// being selected.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/Registry.hpp"
#include "core/Result.hpp"
#include "vision/Detection.hpp"

namespace visora::vision {

class ResultSink {
public:
    virtual ~ResultSink() = default;

    virtual std::string_view id() const = 0;

    // Begins accepting results. A sink that cannot start says why and is left
    // out; it must never take the pipeline down with it.
    virtual core::Status start() = 0;
    virtual void stop() = 0;

    // Called on a job worker thread, once per result.
    //
    // MUST NOT BLOCK INDEFINITELY. This runs on the thread that would otherwise
    // be inferring the next frame, so a sink waiting on a slow network stalls
    // the pipeline. Buffer, drop, or time out — do not wait.
    virtual void publish(const Result& result) = 0;

    // Whether anything is listening. A sink with no consumer lets the pipeline
    // skip the expensive JPEG encode entirely, which is most of the cost of
    // producing a result.
    virtual bool hasConsumers() const = 0;
};

core::Registry<ResultSink>& resultSinkRegistry();

// Fans out to every sink that started successfully.
class ResultSinkSet {
public:
    // Starts every available sink. Returns how many started; zero is not an
    // error, it means results go nowhere, which is a legitimate configuration.
    std::size_t startAll();
    void stopAll();

    void publish(const Result& result);

    // True when at least one sink has a consumer — the signal for whether it is
    // worth encoding JPEGs at all.
    bool anyConsumers() const;

    std::vector<std::string> activeIds() const;

private:
    std::vector<std::unique_ptr<ResultSink>> m_sinks;
};

}  // namespace visora::vision
