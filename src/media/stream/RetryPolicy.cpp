#include "media/stream/RetryPolicy.hpp"

#include <algorithm>

namespace visora::media {

std::uint32_t RetryPolicy::delayFor(std::uint32_t attempt) const {
    std::uint64_t delay = initialMs;
    // Loop rather than pow: the cap is checked each step, so a large attempt
    // count cannot overflow on its way to being clamped.
    for (std::uint32_t i = 0; i < attempt && delay < maxMs; ++i) delay *= 2;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(delay, maxMs));
}

}  // namespace visora::media
