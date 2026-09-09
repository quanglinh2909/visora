#pragma once

#include <string>

namespace visora::hal {

// What a backend answers when asked "can you run on this machine?".
//
// `detail` is written for a human reading the capability report — "librga not
// found", "RGA 2D via librga 1.9.3", "no CUDA device". It is the difference
// between a support ticket and a five-second diagnosis.
struct Probe {
    bool available = false;
    std::string detail;

    static Probe yes(std::string detail) { return {true, std::move(detail)}; }
    static Probe no(std::string detail) { return {false, std::move(detail)}; }
};

// A row of the capability report.
struct BackendStatus {
    std::string id;
    std::string kind;     // "image-ops", "inference", ...
    int priority = 0;
    bool available = false;
    bool selected = false;
    std::string detail;
};

}  // namespace visora::hal
