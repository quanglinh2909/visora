#include "media/recording/MotionGate.hpp"

#include <algorithm>
#include <utility>

namespace visora::media {

MotionGate::MotionGate(MotionGateOptions options) : m_options(std::move(options)) {}

void MotionGate::noteEvent(std::int64_t atMs) {
    m_events.push_back(atMs);
    // Kept sorted so pruning is a prefix removal rather than a scan.
    if (m_events.size() > 1 && m_events[m_events.size() - 2] > atMs) {
        std::sort(m_events.begin(), m_events.end());
    }
}

void MotionGate::offer(const RecordingSegment& segment) { m_held.push_back(segment); }

bool MotionGate::anyEventCovers(const RecordingSegment& segment) const {
    for (const std::int64_t at : m_events) {
        MotionWindow window;
        window.startMs = at;
        window.endMs = at;
        window.preMs = static_cast<std::int64_t>(std::max(0, m_options.preSeconds)) * 1000;
        window.postMs = static_cast<std::int64_t>(std::max(0, m_options.postSeconds)) * 1000;
        if (segmentIsWorthKeeping(segment, window)) return true;
    }
    return false;
}

void MotionGate::forgetOldEvents(std::int64_t nowMs) {
    // An event can only still matter to a segment we have not resolved yet, and
    // the oldest unresolved segment starts no earlier than the hold horizon.
    const std::int64_t postMs =
        static_cast<std::int64_t>(std::max(0, m_options.postSeconds)) * 1000;
    const std::int64_t horizon = nowMs - postMs - holdMs();
    const auto stale = std::lower_bound(m_events.begin(), m_events.end(), horizon);
    m_events.erase(m_events.begin(), stale);
}

std::int64_t MotionGate::holdMs() const {
    // A segment is held exactly as long as the pre-roll it could still be saved
    // by. Holding longer delays the delete; holding less loses the very footage
    // pre-roll exists to keep.
    return static_cast<std::int64_t>(std::max(0, m_options.preSeconds)) * 1000;
}

std::vector<GateDecision> MotionGate::settle(std::int64_t nowMs) {
    std::vector<GateDecision> decided;
    std::vector<RecordingSegment> stillHeld;
    stillHeld.reserve(m_held.size());

    for (RecordingSegment& segment : m_held) {
        if (segment.endMs + holdMs() > nowMs) {
            // An event could still arrive that saves this one.
            stillHeld.push_back(std::move(segment));
            continue;
        }
        GateDecision decision;
        decision.keep = anyEventCovers(segment);
        decision.segment = std::move(segment);
        decided.push_back(std::move(decision));
    }

    m_held = std::move(stillHeld);
    forgetOldEvents(nowMs);
    return decided;
}

std::vector<GateDecision> MotionGate::flush() {
    std::vector<GateDecision> decided;
    decided.reserve(m_held.size());
    for (RecordingSegment& segment : m_held) {
        GateDecision decision;
        decision.keep = anyEventCovers(segment);
        decision.segment = std::move(segment);
        decided.push_back(std::move(decision));
    }
    m_held.clear();
    return decided;
}

}  // namespace visora::media
