#include "core/Time.hpp"

#include <ctime>

namespace visora::core {

std::string toIso8601(std::chrono::system_clock::time_point when) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm utc{};
    // The _r form: the shared-buffer gmtime() would be a data race between the
    // streaming worker and a request thread formatting at the same moment.
    gmtime_r(&seconds, &utc);

    char buffer[32];
    const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer, written);
}

}  // namespace visora::core
