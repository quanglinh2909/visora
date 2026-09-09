#pragma once

// One process-wide lock around librga.
//
// The im2d context is not thread-safe: concurrent improcess / importbuffer /
// releasebuffer calls from several pipeline threads corrupt it. Serialising at
// this level costs nothing measurable — the blits themselves are microseconds
// and the hardware queue is serial anyway.

#include <mutex>

namespace visora::hal::rockchip {

inline std::mutex& rgaMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace visora::hal::rockchip
