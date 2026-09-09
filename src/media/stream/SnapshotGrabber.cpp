#include "media/stream/SnapshotGrabber.hpp"

#include <algorithm>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "core/Log.hpp"
#include "media/gst/CodecProvider.hpp"
#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "snapshot";

// How long one pull waits before the loop checks the bus and the deadline again.
constexpr GstClockTime kPullTimeout = 200 * GST_MSECOND;

// Frees a GstSample unless it is null. Written out because the pull loop holds
// two of them and leaking either leaks a whole decoded frame per request.
void unrefSample(GstSample*& sample) {
    if (sample) {
        gst_sample_unref(sample);
        sample = nullptr;
    }
}

class GstSnapshotGrabber : public SnapshotGrabber {
public:
    core::Result<std::vector<std::uint8_t>> grab(const std::string& rtspUrl,
                                                 const SnapshotOptions& options) override {
        if (rtspUrl.empty()) {
            return core::invalidArgument("camera has no RTSP URL");
        }

        const std::string launch = snapshotLaunch(rtspUrl, options);
        if (launch.empty()) {
            return core::unsupported("no JPEG encoder is installed");
        }
        VS_DEBUG(kCategory) << launch;

        // A recoverable parse failure — an unknown property, a link that could
        // not be made — returns a PARTIAL pipeline with err set and elements
        // silently missing. Such a pipeline never produces a frame and never
        // posts a bus error, so it can only fail as a timeout minutes later.
        // Treat any err as fatal while it still says what went wrong.
        GError* err = nullptr;
        GstElement* pipeline = gst_parse_launch(launch.c_str(), &err);
        if (!pipeline || err) {
            const std::string message =
                err && err->message ? err->message : "could not build the snapshot pipeline";
            if (err) g_error_free(err);
            if (pipeline) gst_object_unref(pipeline);
            return core::internalError(message);
        }

        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        if (!sink) {
            gst_object_unref(pipeline);
            return core::internalError("snapshot pipeline has no sink");
        }

        auto result = run(pipeline, sink, options);

        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return result;
    }

private:
    static core::Result<std::vector<std::uint8_t>> run(GstElement* pipeline, GstElement* sink,
                                                       const SnapshotOptions& options) {
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            return core::internalError("could not start the snapshot pipeline");
        }

        GstBus* bus = gst_element_get_bus(pipeline);
        const gint64 deadline =
            g_get_monotonic_time() + static_cast<gint64>(std::max(options.timeoutMs, 1)) * 1000;

        GstSample* keep = nullptr;
        // The newest warm-up frame, kept as a fallback: a stream that ends
        // before warm-up is over still yields a picture, which beats an error.
        GstSample* warmupFallback = nullptr;
        int warmup = std::max(options.warmupFrames, 0);
        std::string error;

        while (g_get_monotonic_time() < deadline) {
            // Fail on a pipeline error immediately instead of waiting out the
            // whole timeout with a message sitting unread on the bus.
            GstMessage* message = gst_bus_pop_filtered(
                bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (message) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                    GError* e = nullptr;
                    gchar* debug = nullptr;
                    gst_message_parse_error(message, &e, &debug);
                    error = e && e->message ? e->message : "snapshot pipeline error";
                    if (e) g_error_free(e);
                    if (debug) g_free(debug);
                } else {
                    error = "the camera stream ended";
                }
                gst_message_unref(message);
                break;
            }

            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), kPullTimeout);
            if (!sample) continue;
            if (warmup > 0) {
                --warmup;
                unrefSample(warmupFallback);
                warmupFallback = sample;
                continue;
            }
            keep = sample;
            break;
        }

        if (!keep && warmupFallback) {
            keep = warmupFallback;
            warmupFallback = nullptr;
            error.clear();  // we have a picture; the EOS no longer matters
        }
        unrefSample(warmupFallback);
        gst_object_unref(bus);

        if (!keep) {
            return core::hardwareFailure(
                error.empty() ? "timed out waiting for a camera frame" : error);
        }

        std::vector<std::uint8_t> jpeg;
        GstBuffer* buffer = gst_sample_get_buffer(keep);
        GstMapInfo info;
        if (buffer && gst_buffer_map(buffer, &info, GST_MAP_READ)) {
            jpeg.assign(info.data, info.data + info.size);
            gst_buffer_unmap(buffer, &info);
        }
        unrefSample(keep);

        if (jpeg.empty()) return core::internalError("the snapshot encoder produced no data");
        return jpeg;
    }
};

}  // namespace

std::string snapshotLaunch(const std::string& rtspUrl, const SnapshotOptions& options) {
    const auto jpeg = resolveJpegEncoder(options.quality);
    if (!jpeg) return {};

    const int latency = std::max(options.latencyMs, kSnapshotMinimumLatencyMs);

    LaunchPipeline pipeline;
    pipeline.chain()
        // No drop-on-latency. It is false by default and must stay that way:
        // dropping the tail of a burst-delivered keyframe is exactly what turns
        // a snapshot green.
        .add(ElementSpec("rtspsrc")
                 .named("src")
                 .setQuoted("location", rtspUrl)
                 .set("latency", latency)
                 .set("protocols", "tcp"))
        .caps("application/x-rtp,media=video")
        .add("decodebin")
        .caps("video/x-raw")
        .add("videoconvert")
        .add(jpeg->spec)
        // max-buffers=1 with drop=false is what makes warm-up a frame count:
        // the decoder advances exactly one frame per pull.
        .add(ElementSpec("appsink")
                 .named("sink")
                 .set("max-buffers", 1)
                 .set("drop", false)
                 .set("sync", false));

    // NOT wrapped: gst_parse_launch returns a GstBin rather than a GstPipeline
    // for a parenthesised description, and set_state on that goes nowhere.
    return pipeline.toLaunch(/*wrapped=*/false);
}

std::shared_ptr<SnapshotGrabber> makeGstSnapshotGrabber() {
    return std::make_shared<GstSnapshotGrabber>();
}

}  // namespace visora::media
