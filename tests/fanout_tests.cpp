// The GOP cache and the consumer fan-out.
//
// Written against real GstBuffers because the thing being tested is reference
// ownership as much as ordering: a cache that leaks a buffer per frame takes a
// week to show up as a memory problem on a board and ten seconds to show up
// here.

#include "TestHarness.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "media/source/SinkFanout.hpp"

using namespace visora;
using visora::media::SinkFanout;
using visora::media::SinkOptions;

namespace {

// A buffer carrying one identifying byte, so a test can assert WHICH frames
// arrived and in what order rather than merely how many.
GstBuffer* frameNamed(std::uint8_t id, bool keyframe, std::size_t size = 16) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    GstMapInfo map;
    gst_buffer_map(buffer, &map, GST_MAP_WRITE);
    map.data[0] = id;
    gst_buffer_unmap(buffer, &map);
    if (!keyframe) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    return buffer;
}

std::uint8_t idOf(GstBuffer* buffer) {
    GstMapInfo map;
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    const std::uint8_t id = map.data[0];
    gst_buffer_unmap(buffer, &map);
    return id;
}

// Pushes one frame and drops the test's own reference, so anything still held
// afterwards is held by the cache.
void push(SinkFanout& fanout, GstBuffer* buffer, GstCaps* caps) {
    const bool keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    fanout.deliver(buffer, caps, keyframe);
    gst_buffer_unref(buffer);
}

struct Recorder {
    std::vector<std::uint8_t> seen;
    media::EncodedSource::Sink sink() {
        return [this](GstBuffer* buffer, GstCaps*) { seen.push_back(idOf(buffer)); };
    }
};

}  // namespace

VS_TEST(a_new_consumer_sees_the_current_gop_immediately) {
    // The whole point. Before this, a consumer joining between keyframes got
    // nothing until the next one — 1.0 to 2.1 seconds on this deployment's
    // cameras, measured — and the tile stayed black for that long.
    SinkFanout fanout;
    push(fanout, frameNamed(1, true), nullptr);
    push(fanout, frameNamed(2, false), nullptr);
    push(fanout, frameNamed(3, false), nullptr);

    Recorder recorder;
    fanout.add(recorder.sink());

    // The keyframe first, then the frames that depend on it, in order.
    VS_CHECK(recorder.seen == (std::vector<std::uint8_t>{1, 2, 3}));
}

VS_TEST(a_primed_consumer_continues_into_the_live_stream_without_a_gap) {
    // Priming and live delivery are two paths into one sink, and a frame that
    // falls between them is a decode error the viewer sees.
    SinkFanout fanout;
    push(fanout, frameNamed(1, true), nullptr);
    push(fanout, frameNamed(2, false), nullptr);

    Recorder recorder;
    fanout.add(recorder.sink());
    push(fanout, frameNamed(3, false), nullptr);
    push(fanout, frameNamed(4, false), nullptr);

    VS_CHECK(recorder.seen == (std::vector<std::uint8_t>{1, 2, 3, 4}));
}

VS_TEST(the_cache_holds_one_gop_and_drops_the_previous_one) {
    // What both references do: a completed GOP is no longer the fastest way
    // into the stream, so it goes when the next keyframe starts.
    SinkFanout fanout;
    push(fanout, frameNamed(1, true), nullptr);
    push(fanout, frameNamed(2, false), nullptr);
    push(fanout, frameNamed(3, true), nullptr);
    push(fanout, frameNamed(4, false), nullptr);

    VS_CHECK_EQ(fanout.stats().gopFrames, static_cast<std::size_t>(2));

    Recorder recorder;
    fanout.add(recorder.sink());
    VS_CHECK(recorder.seen == (std::vector<std::uint8_t>{3, 4}));
}

VS_TEST(a_consumer_that_joins_before_any_keyframe_waits_for_one) {
    // Nothing to prime from, so the old behaviour — and it must still be right,
    // because it is what happens on every source in its first second.
    SinkFanout fanout;
    Recorder recorder;
    fanout.add(recorder.sink());

    push(fanout, frameNamed(1, false), nullptr);
    push(fanout, frameNamed(2, false), nullptr);
    VS_CHECK(recorder.seen.empty());

    push(fanout, frameNamed(3, true), nullptr);
    push(fanout, frameNamed(4, false), nullptr);
    VS_CHECK(recorder.seen == (std::vector<std::uint8_t>{3, 4}));
}

VS_TEST(the_analyser_opts_out_of_priming) {
    // Replaying a GOP into the AI tap is a decode burst and NPU work for a
    // moment that has passed. Both references prime every consumer; this one
    // lets a consumer decline.
    SinkFanout fanout;
    push(fanout, frameNamed(1, true), nullptr);
    push(fanout, frameNamed(2, false), nullptr);

    Recorder recorder;
    fanout.add(recorder.sink(), SinkOptions{/*primeFromGop=*/false});
    VS_CHECK(recorder.seen.empty());

    // And it starts at the next keyframe, exactly as before.
    push(fanout, frameNamed(3, false), nullptr);
    push(fanout, frameNamed(4, true), nullptr);
    VS_CHECK(recorder.seen == (std::vector<std::uint8_t>{4}));
}

VS_TEST(a_stream_with_no_keyframe_cannot_grow_the_cache_without_bound) {
    // The failure both references guard against: an ingest that never sends an
    // IDR. SRS caps frames; this caps bytes too, because what decides whether a
    // GOP is 200 KB or 12 MB is the bitrate, not the picture count.
    media::GopCacheLimits limits;
    limits.maxFrames = 4;
    limits.maxBytes = 1024 * 1024;
    SinkFanout fanout(limits);

    push(fanout, frameNamed(1, true), nullptr);
    for (std::uint8_t i = 2; i < 40; ++i) push(fanout, frameNamed(i, false), nullptr);

    // Dropped rather than grown, and not refilled until a keyframe: a partial
    // GOP is not something a decoder can start from.
    VS_CHECK_EQ(fanout.stats().gopFrames, static_cast<std::size_t>(0));

    Recorder recorder;
    fanout.add(recorder.sink());
    VS_CHECK(recorder.seen.empty());

    // A keyframe restores it.
    push(fanout, frameNamed(99, true), nullptr);
    VS_CHECK(recorder.seen == (std::vector<std::uint8_t>{99}));
    VS_CHECK_EQ(fanout.stats().gopFrames, static_cast<std::size_t>(1));
}

VS_TEST(a_byte_limit_stops_a_high_bitrate_gop) {
    media::GopCacheLimits limits;
    limits.maxFrames = 1000;
    limits.maxBytes = 100;
    SinkFanout fanout(limits);

    push(fanout, frameNamed(1, true, /*size=*/64), nullptr);
    VS_CHECK_EQ(fanout.stats().gopBytes, static_cast<std::size_t>(64));
    push(fanout, frameNamed(2, false, /*size=*/64), nullptr);
    VS_CHECK_EQ(fanout.stats().gopFrames, static_cast<std::size_t>(0));
}

VS_TEST(the_cache_can_be_turned_off) {
    // SRS puts it in one line: "set to off for min delay". A primed consumer
    // starts up to one GOP behind live, and a deployment that cares more about
    // latency than about the first picture must be able to say so.
    media::GopCacheLimits limits;
    limits.enabled = false;
    SinkFanout fanout(limits);

    push(fanout, frameNamed(1, true), nullptr);
    push(fanout, frameNamed(2, false), nullptr);
    VS_CHECK_EQ(fanout.stats().gopFrames, static_cast<std::size_t>(0));

    Recorder recorder;
    fanout.add(recorder.sink());
    VS_CHECK(recorder.seen.empty());
}

VS_TEST(nothing_is_leaked_or_freed_early) {
    // Every cached and pending frame holds a reference, and getting that wrong
    // is either a leak that takes a week to notice or a use-after-free that
    // takes a crash. Asserted on the refcount of a buffer the test still holds.
    GstCaps* caps = gst_caps_new_empty_simple("video/x-h264");
    GstBuffer* keyframe = frameNamed(1, true);

    {
        SinkFanout fanout;
        fanout.deliver(keyframe, caps, true);
        // The test's reference plus the cache's.
        VS_CHECK_EQ(GST_MINI_OBJECT_REFCOUNT_VALUE(keyframe), 2u);

        Recorder recorder;
        fanout.add(recorder.sink());
        // Priming took a reference and gave it back.
        VS_CHECK_EQ(GST_MINI_OBJECT_REFCOUNT_VALUE(keyframe), 2u);

        fanout.clear();
        VS_CHECK_EQ(GST_MINI_OBJECT_REFCOUNT_VALUE(keyframe), 1u);
    }

    VS_CHECK_EQ(GST_MINI_OBJECT_REFCOUNT_VALUE(keyframe), 1u);
    gst_buffer_unref(keyframe);
    gst_caps_unref(caps);
}

VS_TEST(a_consumer_joining_while_frames_flow_still_sees_them_in_order) {
    // The race the design exists for: priming runs outside the lock so a slow
    // new consumer cannot stall the source, which means live frames arrive
    // DURING it. They are held and flushed after the GOP, never interleaved.
    SinkFanout fanout;
    push(fanout, frameNamed(1, true), nullptr);

    std::atomic<bool> go{false};
    std::vector<std::uint8_t> seen;
    std::thread producer([&] {
        while (!go.load()) std::this_thread::yield();
        for (std::uint8_t i = 2; i < 60; ++i) push(fanout, frameNamed(i, false), nullptr);
    });

    go.store(true);
    fanout.add([&seen](GstBuffer* buffer, GstCaps*) { seen.push_back(idOf(buffer)); });
    producer.join();
    // Anything after the handover arrives on the producer's thread; it has
    // joined, so the vector is settled.

    VS_CHECK(!seen.empty());
    VS_CHECK_EQ(seen.front(), static_cast<std::uint8_t>(1));
    // Strictly increasing: no duplicate from being both cached and live, no
    // frame delivered ahead of the GOP it depends on.
    for (std::size_t i = 1; i < seen.size(); ++i) VS_CHECK(seen[i] > seen[i - 1]);
}

int main() {
    gst_init(nullptr, nullptr);
    return ::visora::test::run();
}
