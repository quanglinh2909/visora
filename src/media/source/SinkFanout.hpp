#pragma once

// The consumers attached to one live source, and the GOP that lets a new one
// see a picture immediately.
//
// WHY THIS EXISTS. Encoded video only decodes from a keyframe, so a consumer
// handed a P-frame first reconstructs it against references it never received.
// The obvious answer — and what this system did — is to make a new consumer
// WAIT for the next keyframe. SRS states the cost of that plainly: "if disabled
// the gop cache, the client will wait for the next keyframe for h264, and will
// be black-screen."
//
// Measured on this deployment's own cameras, the wait is 1.0 to 2.1 seconds.
// That is how long every tile on a live wall stays black after it is opened,
// every time.
//
// So keep the current GOP instead. A new consumer is handed the frames since
// the last keyframe and then joins the live stream, and the first picture is
// immediate. Both reference servers do exactly this — SrsGopCache in SRS, the
// keyed RingBuffer in ZLMediaKit — and both agree on the rules: cache from each
// keyframe, drop the previous GOP when the next one starts, and cap the cache
// so a stream that never sends a keyframe cannot exhaust memory.
//
// THE HONEST COST. A consumer primed from the cache starts up to one GOP behind
// live and only catches up as fast as its player drains. SRS says the same in
// one line — "set to off for min delay" — so this is a choice, not a free win,
// and it is configurable. It is on by default because a VMS live wall is judged
// on whether the picture appears, and one to two seconds of latency against an
// RTSP camera is normal.
//
// WHERE THIS GOES FURTHER THAN THE REFERENCES. Both prime every player. Here
// priming is PER CONSUMER, because not every consumer wants it: replaying two
// seconds of frames into the AI tap is a burst of stale pictures through a
// decoder and an NPU to produce detections that are already history. Live
// viewers prime; the analyser does not.
//
// And the cache is capped by BYTES as well as by frame count. SRS caps frames
// only, which does not bound memory: the thing that decides whether a GOP is
// 200 KB or 12 MB is the bitrate, not the number of pictures.

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "media/source/EncodedSource.hpp"

typedef struct _GstBuffer GstBuffer;
typedef struct _GstCaps GstCaps;

namespace visora::media {

struct GopCacheLimits {
    // Off entirely. Every consumer then waits for the next keyframe, which is
    // what this system did before and what SRS calls the minimum-delay setting.
    bool enabled = true;

    // Frames in one cached GOP. A camera at 25 fps with a two-second keyframe
    // interval caches 50, so this is generous headroom rather than a target. A
    // stream that exceeds it is one that is not sending keyframes, and the
    // cache is dropped rather than grown.
    std::size_t maxFrames = 300;

    // Bytes in one cached GOP. The real bound: 1080p at 8 Mbps is about 2 MB of
    // GOP, and a camera misconfigured to a much higher bitrate must not be able
    // to take the process down.
    std::size_t maxBytes = 4u * 1024 * 1024;
};

struct FanoutStats {
    std::size_t consumers = 0;
    std::size_t gopFrames = 0;
    std::size_t gopBytes = 0;
    // Consumers that got a picture immediately, and those that had to wait for
    // a keyframe because the cache was empty or unusable.
    std::uint64_t primed = 0;
    std::uint64_t waited = 0;
};

class SinkFanout {
public:
    // `tag` names the stream in the log — a camera id here. A fan-out that
    // cannot say WHICH stream primed a viewer is not much use on a board
    // running seventeen of them.
    explicit SinkFanout(GopCacheLimits limits = {}, std::string tag = {});
    ~SinkFanout();

    SinkFanout(const SinkFanout&) = delete;
    SinkFanout& operator=(const SinkFanout&) = delete;

    std::uint64_t add(EncodedSource::Sink sink, SinkOptions options = {});
    void remove(std::uint64_t id);

    // Called on the source's streaming thread. Updates the cache, then hands
    // the frame to every consumer that is ready for it.
    //
    // Sinks are invoked OUTSIDE the lock: a sink pushes into another pipeline's
    // appsrc, and holding the lock across that would let one slow consumer
    // block another consumer's remove(). This is the steady-state path and the
    // reason that matters; add() primes under the lock instead, where the burst
    // is bounded and the alternative was a race — see its comment.
    void deliver(GstBuffer* buffer, GstCaps* caps, bool keyframe);

    // Drops the cache and every consumer. For a source that has stopped.
    void clear();

    std::size_t size() const;
    FanoutStats stats() const;

private:
    // One cached frame, holding its own references. Caps travel with the frame
    // rather than being taken once: a source can renegotiate mid-stream, and a
    // replayed frame described by the wrong caps is worse than no frame.
    struct Frame {
        GstBuffer* buffer = nullptr;
        GstCaps* caps = nullptr;
    };

    struct Consumer {
        EncodedSource::Sink sink;
        // Waiting for a keyframe to start at, because this consumer did not
        // prime. The behaviour this class replaces, kept for the analyser and
        // as the fallback whenever the cache cannot serve.
        bool waitingForKeyframe = true;
    };

    static Frame retain(GstBuffer* buffer, GstCaps* caps);
    static void release(Frame& frame);
    static void releaseAll(std::vector<Frame>& frames);

    void dropGop();

    const GopCacheLimits m_limits;
    const std::string m_tag;

    mutable std::mutex m_mutex;
    std::map<std::uint64_t, Consumer> m_consumers;
    std::uint64_t m_nextId = 1;

    std::vector<Frame> m_gop;
    std::size_t m_gopBytes = 0;
    // The current GOP overflowed a limit, so it was dropped and nothing more is
    // cached until the next keyframe starts a fresh one. Without this a stream
    // with no keyframes would refill and re-drop the cache on every frame.
    bool m_gopOverflowed = false;

    std::uint64_t m_primed = 0;
    std::uint64_t m_waited = 0;
};

}  // namespace visora::media
