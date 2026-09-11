#pragma once

// When to let go of a stream nobody is watching.
//
// Releasing it the instant the last consumer leaves is what this system did,
// and it is right for a board running seventeen cameras: an unwatched camera
// should not cost a connection, a jitterbuffer and a parser. It is wrong for
// the thing people do most, which is reload the page. That costs a full RTSP
// re-handshake — 300 to 800 ms against the cameras here — for a viewer who
// never really left.
//
// Both reference servers keep a grace period for exactly this:
// `streamNoneReaderDelayMS` in ZLMediaKit, the publish timeouts in SRS. So does
// this, and the rule lives here on its own so it can be asserted without a
// camera, a pipeline or a clock that really ticks.

#include <chrono>
#include <optional>

namespace visora::media {

class IdleTimer {
public:
    using Clock = std::chrono::steady_clock;

    explicit IdleTimer(std::chrono::milliseconds linger) : m_linger(linger) {}

    // Call on every sweep with whether anything is consuming right now.
    // Returns true the first time the grace period has run out, which is the
    // caller's cue to retire the stream.
    bool expired(bool hasConsumers, Clock::time_point now) {
        if (hasConsumers) {
            // Back in use. A viewer returning within the window gets the stream
            // that is already running, which is the whole point.
            m_idleSince.reset();
            return false;
        }
        if (!m_idleSince.has_value()) {
            m_idleSince = now;
            // A zero grace period means the old behaviour — go at once — and
            // it must not take an extra sweep to do it.
            return m_linger <= std::chrono::milliseconds::zero();
        }
        return now - *m_idleSince >= m_linger;
    }

    bool idle() const { return m_idleSince.has_value(); }

    // How long it has been unwatched, for reporting.
    std::chrono::milliseconds idleFor(Clock::time_point now) const {
        if (!m_idleSince.has_value()) return std::chrono::milliseconds::zero();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - *m_idleSince);
    }

private:
    std::chrono::milliseconds m_linger;
    std::optional<Clock::time_point> m_idleSince;
};

}  // namespace visora::media
