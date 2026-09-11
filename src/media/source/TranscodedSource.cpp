#include "media/source/TranscodedSource.hpp"

#include "media/source/SinkFanout.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "core/Log.hpp"
#include "media/gst/BusWatcher.hpp"
#include "media/gst/CodecProvider.hpp"
#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "source";
constexpr const char* kAppSrcName = "tc_src";
constexpr const char* kAppSinkName = "tc_out";

// Same reasoning as the shared source: enough to ride out a stall, and drop is
// false because losing an access unit corrupts the stream for every consumer
// rather than only the slow one.
constexpr int kAppsinkMaxBuffers = 30;
constexpr gint64 kBitrateWarmupUs = 2 * G_USEC_PER_SEC;

}  // namespace

struct TranscodedSource::Impl {
    Impl(GopCacheLimits limits, std::string tag) : fanout(limits, std::move(tag)) {}

    TranscodedSource* owner = nullptr;
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* appsink = nullptr;
    BusWatcher bus;

    std::atomic<bool> alive{false};
    std::atomic<std::uint64_t> bitrateBps{0};
    std::uint64_t rateBytes = 0;
    gint64 rateSinceUs = 0;
    // The first timestamp OUT of the encoder, so the stream this source
    // publishes starts at zero like the one it wraps.
    GstClockTime outBase = GST_CLOCK_TIME_NONE;

    // The consumers of this transcode, with the GOP that starts a new one at
    // once. It carries its own lock.
    SinkFanout fanout;

    // Guards the FEED — the push side into this pipeline's appsrc. Same
    // discipline as everywhere else that pushes into an appsrc from another
    // pipeline's thread: `feeding` under the lock is what makes teardown safe
    // rather than merely unlikely.
    std::mutex mutex;
    bool feeding = false;
    bool capsSet = false;
    // The first upstream timestamp, so this pipeline's timeline starts at zero.
    GstClockTime basePts = GST_CLOCK_TIME_NONE;

    void push(GstBuffer* buffer, GstCaps* caps);
    void deliver(GstSample* sample);
};

void TranscodedSource::Impl::push(GstBuffer* buffer, GstCaps* caps) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!feeding || appsrc == nullptr) return;

    if (!capsSet && caps != nullptr) {
        gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
        capsSet = true;
    }

    // A shallow copy: the buffer is shared with every other consumer of the
    // upstream source and appsrc is about to write a timestamp onto it.
    GstBuffer* out = gst_buffer_make_writable(gst_buffer_ref(buffer));

    // REBASED, not restamped — the same choice the RTSP restream makes, and it
    // has to be the same or two feeds of one camera carry two different
    // timelines. The upstream clock has another base, so its values cannot pass
    // through; the SPACING between them is the camera's real frame timing and
    // is worth keeping. Letting do-timestamp use the local clock instead put
    // the first frame at 3,600 seconds, because that is where a fresh
    // pipeline's running time starts.
    const GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (GST_CLOCK_TIME_IS_VALID(pts)) {
        if (!GST_CLOCK_TIME_IS_VALID(basePts) || pts < basePts) basePts = pts;
        GST_BUFFER_PTS(out) = pts - basePts;
        const GstClockTime dts = GST_BUFFER_DTS(buffer);
        GST_BUFFER_DTS(out) =
            GST_CLOCK_TIME_IS_VALID(dts) && dts >= basePts ? dts - basePts : GST_BUFFER_PTS(out);
    } else {
        // No timestamp at all: cleared so do-timestamp applies this pipeline's
        // clock, which only stamps a buffer that has none.
        GST_BUFFER_PTS(out) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(out) = GST_CLOCK_TIME_NONE;
    }
    GST_BUFFER_DURATION(out) = GST_CLOCK_TIME_NONE;
    gst_app_src_push_buffer(GST_APP_SRC(appsrc), out);
}

void TranscodedSource::Impl::deliver(GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    if (!buffer) return;

    const bool keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    // NORMALISE THE PUBLISHED TIMELINE, and do it on the way out rather than
    // only on the way in.
    //
    // Rebasing the encoder's INPUT is not enough: measured on this chain, the
    // first buffer goes in at 0 and comes out at 3,600,000 seconds. Something
    // between the parser and the encoder carries a constant offset of its own,
    // and chasing which element it is would fix this pipeline and not the next
    // one. What matters is the contract: a consumer of an EncodedSource gets
    // one timeline whether or not a transcode happened, and a passthrough
    // source starts near zero. So does this.
    GstBuffer* stamped = buffer;
    GstBuffer* owned = nullptr;
    const GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (GST_CLOCK_TIME_IS_VALID(pts)) {
        if (!GST_CLOCK_TIME_IS_VALID(outBase) || pts < outBase) outBase = pts;
        if (outBase > 0) {
            owned = gst_buffer_make_writable(gst_buffer_ref(buffer));
            GST_BUFFER_PTS(owned) = pts - outBase;
            const GstClockTime dts = GST_BUFFER_DTS(buffer);
            GST_BUFFER_DTS(owned) = GST_CLOCK_TIME_IS_VALID(dts) && dts >= outBase
                                        ? dts - outBase
                                        : GST_BUFFER_PTS(owned);
            stamped = owned;
        }
    }

    const gint64 nowUs = g_get_monotonic_time();
    rateBytes += gst_buffer_get_size(buffer);
    if (rateSinceUs == 0) {
        rateSinceUs = nowUs;
    } else if (nowUs - rateSinceUs >= kBitrateWarmupUs) {
        bitrateBps.store(rateBytes * 8 * G_USEC_PER_SEC /
                         static_cast<std::uint64_t>(nowUs - rateSinceUs));
    }

    fanout.deliver(stamped, caps, keyframe);
    if (owned) gst_buffer_unref(owned);
}

namespace {

GstFlowReturn onNewSample(GstAppSink* sink, gpointer user) {
    auto* impl = static_cast<TranscodedSource::Impl*>(user);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    impl->deliver(sample);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void handleBusMessage(TranscodedSource::Impl* impl, const std::string& cameraId,
                      GstMessage* message) {
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR:
            VS_WARN(kCategory) << cameraId << ": transcode failed: " << errorTextOf(message);
            impl->alive.store(false);
            break;
        case GST_MESSAGE_EOS:
            VS_WARN(kCategory) << cameraId << ": transcode ended";
            impl->alive.store(false);
            break;
        default:
            break;
    }
}

}  // namespace

TranscodedSource::TranscodedSource(std::string cameraId,
                                   std::shared_ptr<EncodedSource> upstream,
                                   TranscodeOptions options)
    : m_cameraId(std::move(cameraId)),
      m_upstream(std::move(upstream)),
      m_options(options),
      m_impl(std::make_unique<Impl>(m_options.gopCache, m_cameraId + " (h264)")) {
    m_impl->owner = this;
}

TranscodedSource::~TranscodedSource() { stop(); }

std::string TranscodedSource::launchFor(Codec sourceCodec, const TranscodeOptions& options) {
    if (sourceCodec == Codec::Unknown) return {};

    EncoderParams params;
    params.lowLatency = true;
    params.gopSize = options.gopSize;
    params.bitrateKbps = options.bitrateKbps;

    // Through the codec providers, so this is MPP on a Rockchip board, NVENC on
    // an NVIDIA machine and libav where there is nothing else. Naming the
    // elements here is what tied the predecessor to one board.
    const auto decoder = resolveDecoder(sourceCodec);
    const auto encoder = resolveEncoder(Codec::H264, params);
    if (!decoder || !encoder) return {};

    LaunchPipeline pipeline;
    pipeline.chain()
        .add(ElementSpec("appsrc")
                 .named(kAppSrcName)
                 .set("is-live", true)
                 .set("format", "time")
                 .set("do-timestamp", false)
                 .set("max-bytes", 0)
                 .set("block", false))
        .caps(parsedCaps(sourceCodec))
        .add(ElementSpec(parser(sourceCodec)).set("config-interval", -1))
        .add(decoder->spec)
        // videoconvert between the decoder and the encoder's format. A hardware
        // decoder does not necessarily produce what the encoder wants and often
        // cannot be asked to; where the two already agree it negotiates to
        // passthrough and costs nothing, so this does not give up zero-copy.
        .add("videoconvert")
        .caps("video/x-raw,format=" + encoderInputFormatFor(Codec::H264, params))
        .add(encoder->spec)
        // config-interval=-1 puts the parameter sets in every keyframe, which
        // is what lets a viewer attaching mid-stream decode from the first one
        // it receives.
        .add(ElementSpec(parser(Codec::H264)).set("config-interval", -1))
        .caps(parsedCaps(Codec::H264))
        .add(ElementSpec("appsink")
                 .named(kAppSinkName)
                 .set("sync", false)
                 .set("max-buffers", kAppsinkMaxBuffers)
                 .set("drop", false));

    return pipeline.toLaunch(/*wrapped=*/false);
}

core::Status TranscodedSource::start() {
    if (m_impl->pipeline) return {};
    if (!m_upstream) return core::invalidArgument("a transcode needs a source to read");
    if (m_upstream->codec() == Codec::H264) {
        // Nothing to do, and building a decode/encode pass to do it would be
        // the most expensive no-op in the program.
        return core::invalidArgument("this source is already H.264");
    }

    TranscodeOptions options = m_options;
    if (options.bitrateKbps == 0) {
        const std::uint64_t measured = m_upstream->bitrateBps();
        if (measured > 0) options.bitrateKbps = static_cast<int>(measured / 1000);
    }

    const std::string launch = launchFor(m_upstream->codec(), options);
    if (launch.empty()) {
        return core::unsupported("no transcode pipeline from " +
                                 std::string(toString(m_upstream->codec())) +
                                 " to H.264 on this machine");
    }
    VS_DEBUG(kCategory) << m_cameraId << ": " << launch;

    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(launch.c_str(), &error);
    if (!m_impl->pipeline || error) {
        const std::string message =
            error && error->message ? error->message : "could not build the transcode pipeline";
        if (error) g_error_free(error);
        stop();
        return core::internalError(message);
    }

    m_impl->appsrc = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), kAppSrcName);
    m_impl->appsink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), kAppSinkName);
    if (!m_impl->appsrc || !m_impl->appsink) {
        stop();
        return core::internalError("the transcode pipeline is missing its endpoints");
    }

    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = &onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_impl->appsink), &callbacks, m_impl.get(), nullptr);

    m_impl->bus.start(m_impl->pipeline, m_cameraId,
                      [impl = m_impl.get(), id = m_cameraId](GstMessage* message) {
                          handleBusMessage(impl, id, message);
                      });

    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        stop();
        return core::hardwareFailure("could not start the transcode for " + m_cameraId);
    }

    m_impl->alive.store(true);
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->feeding = true;
    }
    // Attached LAST, once the pipeline is playing: buffers arriving at an
    // appsrc in a pipeline that is not yet running queue up, and the first
    // keyframe out the far end is late by however long that took.
    m_sinkId = m_upstream->addSink(
        [impl = m_impl.get()](GstBuffer* buffer, GstCaps* caps) { impl->push(buffer, caps); });

    VS_INFO(kCategory) << m_cameraId << ": transcoding "
                       << toString(m_upstream->codec()) << " to h264 ("
                       << (options.bitrateKbps > 0 ? std::to_string(options.bitrateKbps) + " kbps"
                                                   : std::string("encoder default"))
                       << ')';
    return {};
}

void TranscodedSource::stop() {
    // Stop accepting buffers, then detach, then tear down. Detaching first
    // would leave a push in flight against an appsrc being destroyed.
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->feeding = false;
        m_impl->capsSet = false;
        m_impl->basePts = GST_CLOCK_TIME_NONE;
        m_impl->outBase = GST_CLOCK_TIME_NONE;
    }
    if (m_upstream && m_sinkId != 0) {
        m_upstream->removeSink(m_sinkId);
        m_sinkId = 0;
    }

    m_impl->alive.store(false);
    m_impl->bus.stop();
    if (m_impl->pipeline) gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
    if (m_impl->appsrc) {
        gst_object_unref(m_impl->appsrc);
        m_impl->appsrc = nullptr;
    }
    if (m_impl->appsink) {
        gst_object_unref(m_impl->appsink);
        m_impl->appsink = nullptr;
    }
    if (m_impl->pipeline) {
        gst_object_unref(m_impl->pipeline);
        m_impl->pipeline = nullptr;
    }
}

std::uint64_t TranscodedSource::addSink(Sink sink, SinkOptions options) {
    return m_impl->fanout.add(std::move(sink), options);
}

void TranscodedSource::removeSink(std::uint64_t id) { m_impl->fanout.remove(id); }

std::size_t TranscodedSource::sinkCount() const { return m_impl->fanout.size(); }

// Dead when either end is: a transcode of a source that has gone is not a
// stream, it is a pipeline waiting for buffers that will not arrive.
bool TranscodedSource::alive() const {
    return m_impl->alive.load() && m_upstream && m_upstream->alive();
}

std::uint64_t TranscodedSource::bitrateBps() const { return m_impl->bitrateBps.load(); }

}  // namespace visora::media
