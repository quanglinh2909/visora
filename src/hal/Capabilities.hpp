#pragma once

// What this build can actually do on this machine.
//
// Printed at startup and served over HTTP once the API layer exists. The point
// is that "why is it slow / why is AI off on this box" is answered by reading
// one table, not by rebuilding with printf.

#include <string>
#include <vector>

#include "hal/Probe.hpp"

namespace visora::hal {

struct Capabilities {
    std::string osName;
    std::string architecture;

    std::string imageOpsSelected;   // empty when none available
    std::string imageOpsProblem;    // why, when none available
    std::string inferenceSelected;
    std::string inferenceProblem;

    std::vector<BackendStatus> backends;  // every kind, highest priority first

    bool aiEnabled() const { return !inferenceSelected.empty(); }
};

// Probes everything. Side-effect free apart from the backend probes themselves.
Capabilities detectCapabilities();

std::string toText(const Capabilities& capabilities);
std::string toJson(const Capabilities& capabilities);

}  // namespace visora::hal
