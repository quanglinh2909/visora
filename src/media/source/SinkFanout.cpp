#include "media/source/SinkFanout.hpp"

#include "core/Log.hpp"

#include <utility>

#include <gst/gst.h>

namespace visora::media {
namespace {

// How many live frames may pile up behind a consumer that is still being primed
// before priming is abandoned.
//
// Priming is a handful of ref-counted pushes and finishes in microseconds, so
// reaching this means the consumer's own sink is blocking. Dropping back to
// "wait for the next keyframe" is the right failure: it costs that one consumer
// a keyframe interval and costs everyone else nothing, where growing the queue
// would spend memory to hide a problem.
constexpr std::size_t kMaxPendingWhilePriming = 256;

constexpr const char* kCategory = "source";

}  // namespace

SinkFanout::SinkFanout(GopCacheLimits limits, std::string tag)
    : m_limits(limits), m_tag(std::move(tag)) {}

SinkFanout::~SinkFanout() { clear(); }

SinkFanout::Frame SinkFanout::retain(GstBuffer* buffer, GstCaps* caps) {
    Frame frame;
    frame.buffer = gst_buffer_ref(buffer);
    frame.caps = caps ? gst_caps_ref(caps) : nullptr;
    return frame;
}

void SinkFanout::release(Frame& frame) {
    if (frame.buffer) gst_buffer_unref(frame.buffer);
    if (frame.caps) gst_caps_unref(frame.caps);
    frame.buffer = nullptr;
    frame.caps = nullptr;
}

void SinkFanout::releaseAll(std::vector<Frame>& frames) {
    for (Frame& frame : frames) release(frame);
    frames.clear();
}

void SinkFanout::sendAndRelease(const EncodedSource::Sink& sink, std::vector<Frame>& frames) {
    for (Frame& frame : frames) {
        sink(frame.buffer, frame.caps);
        release(frame);
    }
    frames.clear();
}

void SinkFanout::dropGop() {
    releaseAll(m_gop);
    m_gopBytes = 0;
}

std::uint64_t SinkFanout::add(EncodedSource::Sink sink, SinkOptions options) {
    std::vector<Frame> snapshot;
    std::uint64_t id = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        id = m_nextId++;

        const bool canPrime = options.primeFromGop && m_limits.enabled && !m_gop.empty();
        Consumer consumer;
        consumer.sink = std::move(sink);
        consumer.priming = canPrime;
        // Not priming means the old behaviour: nothing until a keyframe.
        consumer.waitingForKeyframe = !canPrime;
        m_consumers.emplace(id, std::move(consumer));

        if (canPrime) {
            snapshot.reserve(m_gop.size());
            for (const Frame& frame : m_gop) snapshot.push_back(retain(frame.buffer, frame.caps));
            ++m_primed;
        } else {
            ++m_waited;
        }
    }

    if (!m_tag.empty()) {
        if (snapshot.empty()) {
            VS_INFO(kCategory) << m_tag << ": consumer " << id
                               << " starts at the next keyframe (no cached gop)";
        } else {
            std::size_t bytes = 0;
            for (const Frame& frame : snapshot) bytes += gst_buffer_get_size(frame.buffer);
            VS_INFO(kCategory) << m_tag << ": consumer " << id << " primed from cached gop ("
                               << snapshot.size() << " frames, " << bytes / 1024 << " KiB)";
        }
    }

    if (snapshot.empty()) return id;

    // The cached GOP goes out first, then whatever arrived while it was going
    // out, and only then does the consumer join the live stream. Doing this
    // outside the lock is what keeps a slow new consumer from stalling the
    // source; doing it in this order is what keeps its frames in order.
    EncodedSource::Sink target;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_consumers.find(id);
        if (it == m_consumers.end()) {
            releaseAll(snapshot);
            return id;
        }
        target = it->second.sink;
    }
    sendAndRelease(target, snapshot);

    while (true) {
        std::vector<Frame> pending;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_consumers.find(id);
            // Removed while priming — its pending frames go with it.
            if (it == m_consumers.end()) return id;
            if (it->second.pending.empty()) {
                it->second.priming = false;
                return id;
            }
            pending.swap(it->second.pending);
        }
        sendAndRelease(target, pending);
    }
}

void SinkFanout::remove(std::uint64_t id) {
    std::vector<Frame> orphaned;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_consumers.find(id);
        if (it == m_consumers.end()) return;
        orphaned.swap(it->second.pending);
        m_consumers.erase(it);
    }
    releaseAll(orphaned);
}

void SinkFanout::deliver(GstBuffer* buffer, GstCaps* caps, bool keyframe) {
    if (!buffer) return;

    std::vector<EncodedSource::Sink> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_limits.enabled) {
            if (keyframe) {
                // The previous GOP is complete and no longer the fastest way
                // into the stream. Both references do exactly this.
                dropGop();
                m_gopOverflowed = false;
            }
            if (!m_gopOverflowed) {
                const std::size_t size = gst_buffer_get_size(buffer);
                if (m_gop.size() + 1 > m_limits.maxFrames ||
                    m_gopBytes + size > m_limits.maxBytes) {
                    // A stream that outgrows a whole GOP's worth of cache is one
                    // that is not sending keyframes. Drop what is held and wait
                    // for a keyframe rather than keep a partial GOP nobody can
                    // start from.
                    dropGop();
                    m_gopOverflowed = true;
                } else {
                    m_gop.push_back(retain(buffer, caps));
                    m_gopBytes += size;
                }
            }
        }

        targets.reserve(m_consumers.size());
        for (auto& [id, consumer] : m_consumers) {
            (void)id;
            if (consumer.priming) {
                if (consumer.pending.size() >= kMaxPendingWhilePriming) {
                    releaseAll(consumer.pending);
                    consumer.priming = false;
                    consumer.waitingForKeyframe = true;
                    continue;
                }
                consumer.pending.push_back(retain(buffer, caps));
                continue;
            }
            if (consumer.waitingForKeyframe) {
                if (!keyframe) continue;
                consumer.waitingForKeyframe = false;
            }
            targets.push_back(consumer.sink);
        }
    }

    for (const EncodedSource::Sink& sink : targets) sink(buffer, caps);
}

void SinkFanout::clear() {
    std::map<std::uint64_t, Consumer> consumers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        consumers.swap(m_consumers);
        dropGop();
        m_gopOverflowed = false;
    }
    for (auto& [id, consumer] : consumers) {
        (void)id;
        releaseAll(consumer.pending);
    }
}

std::size_t SinkFanout::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_consumers.size();
}

FanoutStats SinkFanout::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    FanoutStats out;
    out.consumers = m_consumers.size();
    out.gopFrames = m_gop.size();
    out.gopBytes = m_gopBytes;
    out.primed = m_primed;
    out.waited = m_waited;
    return out;
}

}  // namespace visora::media
