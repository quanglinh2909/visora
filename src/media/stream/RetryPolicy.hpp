#pragma once

// How long to wait before trying a camera again.
//
// Pure arithmetic, so the backoff curve is tested rather than observed: the
// predecessor's equivalent could only be checked by unplugging a camera and
// watching the log.

#include <cstdint>

namespace visora::media {

struct RetryPolicy {
    std::uint32_t initialMs = 1000;
    std::uint32_t maxMs = 30000;

    // Doubles per attempt and stops at maxMs. attempt 0 is the first retry.
    std::uint32_t delayFor(std::uint32_t attempt) const;
};

}  // namespace visora::media
