#include "media/gst/BusWatcher.hpp"

#include <utility>

#include <gst/gst.h>

#include "core/Log.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "gst";

// How long a pop waits before the loop checks whether it should stop. Short
// enough that shutdown is not noticeably delayed, long enough that an idle
// pipeline costs nothing measurable.
constexpr GstClockTime kPopTimeout = 200 * GST_MSECOND;

}  // namespace

BusWatcher::~BusWatcher() { stop(); }

void BusWatcher::start(GstElement* pipeline, std::string label, Handler handler) {
    stop();
    if (!pipeline || !handler) return;

    m_pipeline = pipeline;
    m_label = std::move(label);
    m_handler = std::move(handler);
    m_running.store(true);
    m_thread = std::thread([this] { loop(); });
}

void BusWatcher::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
    m_pipeline = nullptr;
    m_handler = nullptr;
}

void BusWatcher::loop() {
    GstBus* bus = gst_element_get_bus(m_pipeline);
    if (!bus) {
        VS_WARN(kCategory) << m_label << ": pipeline has no bus";
        return;
    }

    while (m_running.load()) {
        GstMessage* message = gst_bus_timed_pop(bus, kPopTimeout);
        if (!message) continue;
        // The handler is on this thread, deliberately: a database write here
        // must not be a database write on a streaming thread.
        m_handler(message);
        gst_message_unref(message);
    }

    gst_object_unref(bus);
}

std::string errorTextOf(GstMessage* message) {
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);

    std::string text = error && error->message ? error->message : "unknown GStreamer error";
    if (debug) {
        text += " (";
        text += debug;
        text += ')';
        g_free(debug);
    }
    if (error) g_error_free(error);
    return text;
}

}  // namespace visora::media
