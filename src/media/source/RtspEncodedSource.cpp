#include "media/source/RtspEncodedSource.hpp"

#include "media/source/SinkFanout.hpp"

#include <algorithm>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "core/Log.hpp"
#include "media/gst/BusWatcher.hpp"
#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "source";

// The same floor as the restream and the snapshot, for the same reason: below
// it, a burst-delivered keyframe is truncated and every consumer of this source
// sees a corrupt IDR at once.
constexpr int kMinimumLatencyMs = 300;

// Enough to ride out a stall without turning the source into a buffer. drop is
// false because dropping an access unit here corrupts the stream for every
// consumer, not just the slow one.
constexpr int kAppsinkMaxBuffers = 30;

// Publish a measured bitrate only once there is this much of it. `bps` on a
// hardware encoder can only be set when its pipeline is built, so a consumer
// that re-encodes needs a number BEFORE the first viewer arrives.
constexpr gint64 kBitrateWarmupUs = 2 * G_USEC_PER_SEC;

}  // namespace

struct RtspEncodedSource::Impl {
    Impl(GopCacheLimits limits, std::string tag) : fanout(limits, std::move(tag)) {}

    RtspEncodedSource* owner = nullptr;
    GstElement* pipeline = nullptr;
    GstElement* appsink = nullptr;
    BusWatcher bus;

    std::atomic<bool> alive{false};
    std::atomic<std::uint64_t> bitrateBps{0};
    // Written only by the appsink thread; the result is published atomically
    // because consumers read it from theirs.
    std::uint64_t rateBytes = 0;
    gint64 rateSinceUs = 0;

    SinkFanout fanout;

    void deliver(GstSample* sample);
};

namespace {

gboolean onSelectStream(GstElement*, guint number, GstCaps* caps, gpointer user) {
    auto* self = static_cast<RtspEncodedSource*>(user);
    const GstStructure* structure = caps ? gst_caps_get_structure(caps, 0) : nullptr;
    const gchar* media = structure ? gst_structure_get_string(structure, "media") : nullptr;
    // A camera whose SDP omits `media` gets the benefit of the doubt: declining
    // its only stream would be far worse than setting up an audio one.
    if (!media) return TRUE;
    const gboolean keep = g_strcmp0(media, "video") == 0 ? TRUE : FALSE;
    if (!keep) {
        VS_DEBUG(kCategory) << self->cameraId() << ": declining stream " << number << " (media="
                            << media << ')';
    }
    return keep;
}

GstFlowReturn onNewSample(GstAppSink* appsink, gpointer user) {
    auto* impl = static_cast<RtspEncodedSource::Impl*>(user);
    GstSample* sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_OK;
    impl->deliver(sample);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void handleBusMessage(RtspEncodedSource::Impl* impl, GstMessage* message) {
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR:
            VS_WARN(kCategory) << impl->owner->cameraId() << ": source failed: "
                               << errorTextOf(message);
            // Marked dead rather than torn down here: stopping a pipeline from
            // the thread draining its own bus deadlocks. Whoever holds the
            // source notices it is dead and replaces it.
            impl->alive.store(false);
            break;
        case GST_MESSAGE_EOS:
            VS_WARN(kCategory) << impl->owner->cameraId() << ": source ended";
            impl->alive.store(false);
            break;
        default:
            break;
    }
}

}  // namespace

void RtspEncodedSource::Impl::deliver(GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    if (!buffer) return;

    const bool keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    // Cumulative average, not a sliding window.
    //
    // A one-second window straddling a keyframe measures one and a half to two
    // times the real rate — video has a GOP — and an encoder configured from
    // that spike is worse than one left to guess. A cumulative average cannot
    // spike and converges as it runs.
    const gint64 nowUs = g_get_monotonic_time();
    rateBytes += gst_buffer_get_size(buffer);
    if (rateSinceUs == 0) {
        rateSinceUs = nowUs;
    } else if (nowUs - rateSinceUs >= kBitrateWarmupUs) {
        bitrateBps.store(rateBytes * 8 * G_USEC_PER_SEC /
                         static_cast<std::uint64_t>(nowUs - rateSinceUs));
    }

    fanout.deliver(buffer, caps, keyframe);
}

RtspEncodedSource::RtspEncodedSource(std::string cameraId, std::string rtspUrl, Codec codec,
                                     RtspSourceOptions options)
    : m_cameraId(std::move(cameraId)),
      m_rtspUrl(std::move(rtspUrl)),
      m_codec(codec),
      m_options(std::move(options)),
      m_impl(std::make_unique<Impl>(m_options.gopCache, m_cameraId)) {
    m_impl->owner = this;
}

RtspEncodedSource::~RtspEncodedSource() { stop(); }

std::string RtspEncodedSource::launchFor(const std::string& rtspUrl, Codec codec,
                                         const RtspSourceOptions& options) {
    if (codec == Codec::Unknown) return {};

    LaunchPipeline pipeline;
    pipeline.chain()
        .add(ElementSpec("rtspsrc")
                 .named("src")
                 .setQuoted("location", rtspUrl)
                 .set("latency", std::max(options.latencyMs, kMinimumLatencyMs))
                 .set("protocols", options.protocols))
        .caps(std::string("application/x-rtp,media=video,encoding-name=") + encodingName(codec))
        .add(depayloader(codec))
        // config-interval=-1 repeats the parameter sets with every keyframe, so
        // a consumer that attaches mid-stream can decode from its first
        // keyframe rather than waiting for the camera to resend them.
        .add(ElementSpec(parser(codec)).set("config-interval", -1))
        // Pinning the format here means no consumer has to parse again.
        .caps(parsedCaps(codec))
        .add(ElementSpec("appsink")
                 .named("out")
                 .set("sync", false)
                 .set("max-buffers", kAppsinkMaxBuffers)
                 .set("drop", false));

    // NOT wrapped: gst_parse_launch wants an unparenthesised description.
    return pipeline.toLaunch(/*wrapped=*/false);
}

core::Status RtspEncodedSource::start() {
    if (m_impl->pipeline) return {};

    const std::string launch = launchFor(m_rtspUrl, m_codec, m_options);
    if (launch.empty()) {
        return core::unsupported("no source pipeline for codec " + std::string(toString(m_codec)));
    }
    VS_DEBUG(kCategory) << m_cameraId << ": " << launch;

    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(launch.c_str(), &error);
    if (!m_impl->pipeline || error) {
        const std::string message =
            error && error->message ? error->message : "could not build the source pipeline";
        if (error) g_error_free(error);
        stop();
        return core::internalError(message);
    }

    if (m_options.videoOnly) {
        if (GstElement* src = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "src")) {
            g_signal_connect(src, "select-stream", G_CALLBACK(&onSelectStream), this);
            gst_object_unref(src);
        }
    }

    m_impl->appsink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "out");
    if (!m_impl->appsink) {
        stop();
        return core::internalError("source pipeline has no appsink");
    }

    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = &onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_impl->appsink), &callbacks, m_impl.get(), nullptr);

    m_impl->bus.start(m_impl->pipeline, m_cameraId, [impl = m_impl.get()](GstMessage* message) {
        handleBusMessage(impl, message);
    });

    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        stop();
        return core::hardwareFailure("could not start the source for " + m_cameraId);
    }

    m_impl->alive.store(true);
    VS_INFO(kCategory) << m_cameraId << ": shared source started (" << toString(m_codec) << ')';
    return {};
}

void RtspEncodedSource::stop() {
    m_impl->alive.store(false);
    m_impl->bus.stop();
    if (m_impl->pipeline) {
        gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
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

std::uint64_t RtspEncodedSource::addSink(Sink sink, SinkOptions options) {
    return m_impl->fanout.add(std::move(sink), options);
}

void RtspEncodedSource::removeSink(std::uint64_t id) { m_impl->fanout.remove(id); }

bool RtspEncodedSource::alive() const { return m_impl->alive.load(); }

std::uint64_t RtspEncodedSource::bitrateBps() const { return m_impl->bitrateBps.load(); }

std::size_t RtspEncodedSource::sinkCount() const {
    return m_impl->fanout.size();
}

}  // namespace visora::media
