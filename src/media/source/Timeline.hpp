#pragma once

// Turning a camera's timestamps into a timeline that only ever moves forward.
//
// Cameras lie about time. They restart their clock after a reconnect, jump when
// their NTP sync lands, roll a 32-bit counter over, and occasionally emit a
// single absurd value. Everything downstream believes timestamps: a muxer
// decides where to cut a segment from them, a payloader computes RTP stamps
// from them, and the MoQ wire format carries them to a browser.
//
// ZLMediaKit keeps a whole class for this — `Stamp`, with `MAX_DELTA_STAMP` at
// three seconds, "mainly to prevent network jitter caused by the jump" — and
// SRS has `time_jitter full`, documented as "ensure stream start at zero, and
// ensure stream monotonically increasing". Visora had neither, and paid for it
// twice: a transcode whose output began at 3,600,000 seconds, and PTS handling
// in the restream. Each consumer rebased for itself, so each got to have its own
// version of the bug.
//
// This corrects by OFFSET rather than by rewriting each stamp. That matters:
// with B-frames, PTS and DTS differ per frame and the gap between them is what
// tells a decoder the display order. Shifting both by the same amount preserves
// that exactly, where recomputing PTS would destroy it.
//
// And the clock it watches is DTS, not PTS. DTS is decode order and is
// monotonic by definition; PTS legitimately steps BACKWARD between consecutive
// buffers whenever a stream has B-frames, so a rollback detector watching PTS
// would fire constantly on a perfectly good stream.

#include <cstdint>

namespace visora::media {

class Timeline {
public:
    // Anything further ahead than this is a jump rather than a gap. Three
    // seconds is ZLMediaKit's number and there is no reason to disagree: real
    // network stalls are shorter, and a camera that genuinely produced nothing
    // for three seconds is better treated as having restarted.
    static constexpr std::int64_t kMaxForwardJumpNs = 3'000'000'000;

    // What a stamp advances by when the real delta cannot be trusted and there
    // is no measured one yet. 1/25 s — a frame at the rate these cameras run.
    static constexpr std::int64_t kNominalStepNs = 40'000'000;

    struct Stamps {
        std::int64_t pts = kNone;
        std::int64_t dts = kNone;
    };

    static constexpr std::int64_t kNone = -1;

    // Feeds one buffer's stamps in and gets the corrected pair back. The
    // returned values start at zero for the first frame and never go backward.
    Stamps correct(Stamps in) {
        const std::int64_t clock = in.dts != kNone ? in.dts : in.pts;
        if (clock == kNone) {
            // Nothing to correct against. Passed through: inventing a stamp
            // here would be worse than having none, because a consumer can
            // recognise "none" and cannot recognise "wrong".
            return in;
        }

        if (!m_started) {
            m_started = true;
            m_offset = -clock;
            m_lastClock = clock;
        } else {
            const std::int64_t delta = clock - m_lastClock;
            if (delta < 0 || delta > kMaxForwardJumpNs) {
                // A jump. Carry on from where the timeline had got to, moving
                // by the last delta that made sense, so a reconnecting camera
                // does not drag every consumer's clock with it.
                const std::int64_t step = m_lastGoodDelta > 0 ? m_lastGoodDelta : kNominalStepNs;
                m_offset += step - delta;
                ++m_corrections;
            } else {
                m_lastGoodDelta = delta;
            }
            m_lastClock = clock;
        }

        Stamps out;
        // Both shifted by the same offset, which is what preserves the
        // PTS-to-DTS gap that carries display order.
        if (in.pts != kNone) out.pts = clampToZero(in.pts + m_offset);
        if (in.dts != kNone) out.dts = clampToZero(in.dts + m_offset);
        return out;
    }

    // How many discontinuities have been absorbed. Worth reporting: a camera
    // that produces these steadily is a camera with a problem.
    std::uint64_t corrections() const { return m_corrections; }

private:
    // A B-frame's PTS can sit before the offset origin on the very first
    // frames. Zero rather than a negative stamp, which several GStreamer
    // elements read as "invalid" and drop.
    static std::int64_t clampToZero(std::int64_t value) { return value < 0 ? 0 : value; }

    bool m_started = false;
    std::int64_t m_offset = 0;
    std::int64_t m_lastClock = 0;
    std::int64_t m_lastGoodDelta = 0;
    std::uint64_t m_corrections = 0;
};

}  // namespace visora::media
