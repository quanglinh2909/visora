#pragma once

// What to do about a consumer that cannot keep up.
//
// The tempting answer is to drop the frames that do not fit and carry on. It
// produces a worse result than dropping more: encoded video is a chain, and a
// decoder handed a P-frame whose references were dropped shows tearing and
// smeared blocks until the next keyframe arrives anyway. So the frames after a
// drop are worthless AND expensive — they occupy exactly the bandwidth that is
// already short.
//
// SRS bounds each consumer's queue and "drop the old whole gop" when it
// overflows; ZLMediaKit's keyed ring does the same by construction. Both are
// saying the same thing: once you are behind, stop until the next keyframe and
// resume cleanly there.
//
// The rule is here on its own because it is the kind of thing that is easy to
// state, easy to get subtly wrong, and impossible to test through a socket.

#include <cstdint>

namespace visora::media {

class DropUntilKeyframe {
public:
    // `overflowing` is the consumer's own answer to "am I behind right now".
    // Returns true when the frame should be sent.
    bool admit(bool keyframe, bool overflowing) {
        if (overflowing) {
            // Behind. Everything from here is unusable to the decoder until a
            // keyframe resets it, so stop paying to send it.
            //
            // A keyframe that arrives while overflowing is still refused: it is
            // the largest frame there is, and pushing it into a queue that is
            // already full is how a consumer that was merely behind runs out of
            // memory instead.
            m_waiting = true;
            ++m_dropped;
            return false;
        }
        if (m_waiting) {
            if (!keyframe) {
                ++m_dropped;
                return false;
            }
            // Room again, and a clean place to start.
            m_waiting = false;
        }
        return true;
    }

    bool waiting() const { return m_waiting; }
    std::uint64_t dropped() const { return m_dropped; }

private:
    bool m_waiting = false;
    std::uint64_t m_dropped = 0;
};

// How much undelivered video a consumer may accumulate before it counts as
// behind.
//
// SRS expresses the same bound in SECONDS (`queue_length`, default 30), which
// is the better unit — it is what a viewer experiences. It is not available
// here: an appsrc only reports a meaningful time level when every buffer
// carries a duration, and these come from a camera's parser where that is not
// guaranteed. So bytes, sized to be generous at the bitrates these cameras
// actually send: 4 MB is about sixteen seconds at 2 Mbps and four at 8.
inline constexpr std::uint64_t kMaxQueuedBytes = 4u * 1024 * 1024;

// The same bound for a RECORDER, which is a different trade.
//
// A viewer that falls behind has a slow network and will stay behind; dropping
// early is the kindness. A recorder falls behind because the disk stalled for a
// moment, and dropping there punches a hole in the evidence someone will want
// later. So it is given room to ride out a stall — 32 MB is well over a minute
// at these bitrates — and only drops when the disk is not keeping up at all,
// which is a fault worth the gap it leaves.
inline constexpr std::uint64_t kMaxRecordingQueuedBytes = 32u * 1024 * 1024;

}  // namespace visora::media
