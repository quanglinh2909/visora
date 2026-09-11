#include "media/source/SinkFanout.hpp"

#include "core/Log.hpp"

#include <utility>

#include <gst/gst.h>

namespace visora::media {
namespace {

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

void SinkFanout::dropGop() {
    releaseAll(m_gop);
    m_gopBytes = 0;
}

std::uint64_t SinkFanout::add(EncodedSource::Sink sink, SinkOptions options) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t id = m_nextId++;

    const bool canPrime = options.primeFromGop && m_limits.enabled && !m_gop.empty();
    Consumer consumer;
    consumer.sink = std::move(sink);
    // Not priming means the old behaviour: nothing until a keyframe.
    consumer.waitingForKeyframe = !canPrime;
    m_consumers.emplace(id, std::move(consumer));

    if (!canPrime) {
        ++m_waited;
        if (!m_tag.empty()) {
            VS_INFO(kCategory) << m_tag << ": consumer " << id
                               << " starts at the next keyframe (no cached gop)";
        }
        return id;
    }

    // Primed UNDER THE LOCK, deliberately.
    //
    // The first version did this outside it, to keep a slow new consumer from
    // stalling the source, and needed a per-consumer queue to hold the live
    // frames that arrived meanwhile. That bought a real hazard: between
    // releasing the lock and calling the sink, another thread could remove the
    // consumer and destroy the object the sink's closure points at.
    //
    // Holding the lock removes the hazard and the queue together. The cost is
    // bounded and small — one burst of at most a GOP of pushes into a
    // non-blocking appsrc, once per consumer — where the case the lock-free
    // path exists for is the STEADY state, and deliver() still calls sinks
    // outside the lock.
    const auto& consumerRef = m_consumers.at(id);
    std::size_t bytes = 0;
    for (const Frame& frame : m_gop) {
        bytes += gst_buffer_get_size(frame.buffer);
        consumerRef.sink(frame.buffer, frame.caps);
    }
    ++m_primed;
    if (!m_tag.empty()) {
        VS_INFO(kCategory) << m_tag << ": consumer " << id << " primed from cached gop ("
                           << m_gop.size() << " frames, " << bytes / 1024 << " KiB)";
    }
    return id;
}

void SinkFanout::remove(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consumers.erase(id);
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
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consumers.clear();
    dropGop();
    m_gopOverflowed = false;
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
