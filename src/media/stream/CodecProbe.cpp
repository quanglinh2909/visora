#include "media/stream/CodecProbe.hpp"

#include <gst/gst.h>
#include <gst/rtsp/gstrtsptransport.h>

#include <mutex>
#include <string>

#include "core/Log.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "probe";

struct ProbeState {
    GMainLoop* loop = nullptr;
    Codec codec = Codec::Unknown;
    std::string error;
    bool unsupported = false;
    bool finished = false;

    void finish() {
        if (finished) return;
        finished = true;
        if (loop) g_main_loop_quit(loop);
    }
};

Codec codecFromEncodingName(const gchar* encoding) {
    if (encoding == nullptr) return Codec::Unknown;
    if (g_ascii_strcasecmp(encoding, "H264") == 0) return Codec::H264;
    if (g_ascii_strcasecmp(encoding, "H265") == 0) return Codec::H265;
    return Codec::Unknown;
}

void onPadAdded(GstElement*, GstPad* pad, gpointer userData) {
    auto* state = static_cast<ProbeState*>(userData);

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (caps == nullptr) caps = gst_pad_query_caps(pad, nullptr);
    if (caps == nullptr || gst_caps_is_empty(caps)) {
        if (caps != nullptr) gst_caps_unref(caps);
        return;
    }

    const GstStructure* structure = gst_caps_get_structure(caps, 0);

    // rtspsrc exposes one pad per stream. Only video matters; an audio pad
    // carrying PCMA would otherwise be reported as an unsupported codec and
    // drop the camera into a terminal state that stops reconnects.
    const gchar* media = gst_structure_get_string(structure, "media");
    if (media != nullptr && g_strcmp0(media, "video") != 0) {
        gst_caps_unref(caps);
        return;
    }

    const gchar* encoding = gst_structure_get_string(structure, "encoding-name");
    const Codec codec = codecFromEncodingName(encoding);
    if (codec != Codec::Unknown) {
        state->codec = codec;
        state->finish();
    } else {
        state->unsupported = true;
        state->error = std::string("camera is sending ") +
                       (encoding != nullptr ? encoding : "an unnamed codec") +
                       ", which this build cannot carry";
        state->finish();
    }
    gst_caps_unref(caps);
}

gboolean onBusMessage(GstBus*, GstMessage* message, gpointer userData) {
    auto* state = static_cast<ProbeState*>(userData);
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        // The camera's own words: "Unauthorized", "Could not resolve host".
        // Passing them through is the difference between an operator fixing a
        // password and filing a bug.
        state->error = error != nullptr ? error->message : "RTSP error";
        if (error != nullptr) g_error_free(error);
        if (debug != nullptr) g_free(debug);
        state->finish();
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        state->error = "stream ended before any video was seen";
        state->finish();
    }
    return TRUE;
}

gboolean onTimeout(gpointer userData) {
    auto* state = static_cast<ProbeState*>(userData);
    if (state->error.empty()) state->error = "timed out waiting for the camera to answer";
    state->finish();
    return FALSE;
}

void ensureGstInit() {
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
}

}  // namespace

core::Result<Codec> probeRtspCodec(const std::string& url, const ProbeOptions& options) {
    if (url.empty()) return core::invalidArgument("no RTSP url");
    ensureGstInit();

    // A private main context, so a probe cannot pick up bus messages meant for
    // a running pipeline, and several probes can run at once.
    GMainContext* context = g_main_context_new();
    ProbeState state;
    state.loop = g_main_loop_new(context, FALSE);

    GstElement* pipeline = gst_pipeline_new(nullptr);
    GstElement* source = gst_element_factory_make("rtspsrc", "src");
    if (pipeline == nullptr || source == nullptr) {
        if (pipeline != nullptr) gst_object_unref(pipeline);
        if (source != nullptr) gst_object_unref(source);
        g_main_loop_unref(state.loop);
        g_main_context_unref(context);
        return core::internalError("rtspsrc is not installed");
    }

    g_object_set(source, "location", url.c_str(), "latency", options.latencyMs, "protocols",
                 GST_RTSP_LOWER_TRANS_TCP, "drop-on-latency", TRUE, nullptr);
    gst_bin_add(GST_BIN(pipeline), source);
    g_signal_connect(source, "pad-added", G_CALLBACK(&onPadAdded), &state);

    GstBus* bus = gst_element_get_bus(pipeline);
    GSource* busSource = gst_bus_create_watch(bus);
    g_source_set_callback(busSource, reinterpret_cast<GSourceFunc>(&onBusMessage), &state,
                          nullptr);
    g_source_attach(busSource, context);
    gst_object_unref(bus);

    GSource* timeoutSource = g_timeout_source_new(options.timeoutMs);
    g_source_set_callback(timeoutSource, &onTimeout, &state, nullptr);
    g_source_attach(timeoutSource, context);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        state.error = "could not start the RTSP probe";
    } else {
        g_main_loop_run(state.loop);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_source_destroy(busSource);
    g_source_unref(busSource);
    g_source_destroy(timeoutSource);
    g_source_unref(timeoutSource);
    gst_object_unref(pipeline);
    g_main_loop_unref(state.loop);
    g_main_context_unref(context);

    if (state.codec != Codec::Unknown) {
        VS_DEBUG(kCategory) << url << " is " << toString(state.codec);
        return state.codec;
    }
    if (state.unsupported) return core::unsupported(state.error);
    return core::notFound(state.error.empty() ? "camera did not answer" : state.error);
}

}  // namespace visora::media
