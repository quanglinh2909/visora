#pragma once

// Decides which recorded segments to keep when a camera records ONLY around
// events.
//
// The problem it solves: a segment finishes before anyone can know whether it
// matters. An event a second after the file closed should still save it —
// that is what "pre-motion" means — so the decision has to be deferred, not
// made when the segment closes.
//
// So a closed segment is HELD for the pre-roll window and resolved afterwards:
// kept if any event's window overlaps it, deleted if none does. That deferral
// is the entire mechanism, and it is pure arithmetic, so it is tested here
// rather than observed on a camera.
//
// The event source is deliberately not named. Motion detection provides one;
// so does an AI job that saw a person. Neither is a concept this class needs.

#include <cstdint>
#include <string>
#include <vector>

#include "media/recording/Recording.hpp"

namespace visora::media {

struct MotionGateOptions {
    // How far before an event to keep. Also how long a closed segment is held
    // before its fate is decided — the two are the same number by definition,
    // and letting them differ produced the predecessor's bug where an AI asked
    // for 30 s of pre-roll from a buffer that held 10.
    int preSeconds = 10;
    int postSeconds = 20;
};

// What to do with a segment whose fate is now known.
struct GateDecision {
    RecordingSegment segment;
    bool keep = false;
};

class MotionGate {
public:
    explicit MotionGate(MotionGateOptions options = {});

    // An event happened at this instant. Extends the keep-window; several
    // overlapping events are one window, not several.
    void noteEvent(std::int64_t atMs);

    // A segment finished. It is held, not decided.
    void offer(const RecordingSegment& segment);

    // Resolves every held segment old enough to decide, as of `nowMs`.
    // Everything returned has left the gate; the caller keeps or deletes it.
    std::vector<GateDecision> settle(std::int64_t nowMs);

    // Everything still held, resolved regardless of age. For shutdown, where
    // holding a segment forever means leaking its file.
    std::vector<GateDecision> flush();

    std::size_t heldCount() const { return m_held.size(); }

private:
    // How long a closed segment is held before it is decided.
    std::int64_t holdMs() const;
    bool anyEventCovers(const RecordingSegment& segment) const;
    void forgetOldEvents(std::int64_t nowMs);

    MotionGateOptions m_options;
    std::vector<RecordingSegment> m_held;
    // Event instants, oldest first. Pruned once no future segment can be
    // covered by them, so a camera running for a month does not accumulate a
    // million of them.
    std::vector<std::int64_t> m_events;
};

}  // namespace visora::media
