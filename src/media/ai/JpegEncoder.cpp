#include "media/ai/JpegEncoder.hpp"

#include <cstring>
#include <mutex>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "core/Log.hpp"
#include "media/gst/CodecProvider.hpp"
#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "ai";
constexpr GstClockTime kEncodeTimeout = 2 * GST_SECOND;

}  // namespace

struct JpegEncoder::Impl {
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* appsink = nullptr;
    // Serialises encode(): one pipeline, and two threads pushing into it would
    // interleave frames and pull each other's answers.
    std::mutex mutex;
    core::Size configured;
};

JpegEncoder::JpegEncoder(int quality) : m_quality(quality), m_impl(std::make_unique<Impl>()) {}

JpegEncoder::~JpegEncoder() { stop(); }

std::string JpegEncoder::launchFor(int quality) {
    const auto jpeg = resolveJpegEncoder(quality);
    if (!jpeg) return {};

    LaunchPipeline pipeline;
    pipeline.chain()
        // No caps here: they are set on the appsrc per frame, because the
        // frame size is not known until one arrives and can change when a
        // camera is reconfigured.
        .add(ElementSpec("appsrc").named("src").set("format", "time").set("block", true))
        .add("videoconvert")
        .add(jpeg->spec)
        .add(ElementSpec("appsink").named("out").set("sync", false).set("max-buffers", 1));

    return pipeline.toLaunch(/*wrapped=*/false);
}

core::Status JpegEncoder::start() {
    stop();
    const std::string launch = launchFor(m_quality);
    if (launch.empty()) return core::unsupported("no JPEG encoder is installed");

    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(launch.c_str(), &error);
    if (!m_impl->pipeline || error) {
        const std::string message =
            error && error->message ? error->message : "could not build the JPEG pipeline";
        if (error) g_error_free(error);
        stop();
        return core::internalError(message);
    }

    m_impl->appsrc = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "src");
    m_impl->appsink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "out");
    if (!m_impl->appsrc || !m_impl->appsink) {
        stop();
        return core::internalError("the JPEG pipeline is missing elements");
    }

    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        stop();
        return core::hardwareFailure("could not start the JPEG encoder");
    }
    return {};
}

void JpegEncoder::stop() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
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
    m_impl->configured = core::Size{};
}

core::Result<std::vector<std::uint8_t>> JpegEncoder::encode(const core::ImageView& frame) {
    if (!frame.valid() || !frame.hasCpu()) {
        return core::invalidArgument("the frame has no readable pixels");
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->pipeline) return core::unsupported("the JPEG encoder is not running");

    if (m_impl->configured.width != frame.size.width ||
        m_impl->configured.height != frame.size.height) {
        // Only when the size actually changes: setting caps flushes the
        // encoder, and doing it per frame would undo the point of keeping the
        // pipeline alive.
        GstCaps* caps = gst_caps_new_simple(
            "video/x-raw", "format", G_TYPE_STRING, "NV12", "width", G_TYPE_INT,
            frame.size.width, "height", G_TYPE_INT, frame.size.height, "framerate",
            GST_TYPE_FRACTION, 0, 1, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(m_impl->appsrc), caps);
        gst_caps_unref(caps);
        m_impl->configured = frame.size;
        VS_DEBUG(kCategory) << "jpeg encoder now at " << frame.size.width << 'x'
                            << frame.size.height;
    }

    // A copy, because appsrc takes ownership and the frame belongs to the
    // decoder. Rows are copied one at a time: a hardware decoder pads them to
    // its own alignment, and a straight memcpy of the whole plane would embed
    // that padding as image data.
    const std::size_t ySize =
        static_cast<std::size_t>(frame.size.width) * frame.size.height;
    const std::size_t uvSize = ySize / 2;
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, ySize + uvSize, nullptr);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return core::internalError("could not map the JPEG input buffer");
    }
    const int yStride = frame.planes[0].stride > 0 ? frame.planes[0].stride : frame.size.width;
    const int uvStride =
        frame.planes[1].stride > 0 ? frame.planes[1].stride : frame.size.width;
    for (int row = 0; row < frame.size.height; ++row) {
        std::memcpy(map.data + static_cast<std::size_t>(row) * frame.size.width,
                    frame.data + frame.planes[0].offset +
                        static_cast<std::size_t>(row) * yStride,
                    static_cast<std::size_t>(frame.size.width));
    }
    for (int row = 0; row < frame.size.height / 2; ++row) {
        std::memcpy(map.data + ySize + static_cast<std::size_t>(row) * frame.size.width,
                    frame.data + frame.planes[1].offset +
                        static_cast<std::size_t>(row) * uvStride,
                    static_cast<std::size_t>(frame.size.width));
    }
    gst_buffer_unmap(buffer, &map);

    if (gst_app_src_push_buffer(GST_APP_SRC(m_impl->appsrc), buffer) != GST_FLOW_OK) {
        return core::internalError("the JPEG encoder rejected the frame");
    }

    GstSample* sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(m_impl->appsink), kEncodeTimeout);
    if (!sample) return core::internalError("the JPEG encoder produced nothing");

    std::vector<std::uint8_t> jpeg;
    if (GstBuffer* out = gst_sample_get_buffer(sample)) {
        GstMapInfo outMap;
        if (gst_buffer_map(out, &outMap, GST_MAP_READ)) {
            jpeg.assign(outMap.data, outMap.data + outMap.size);
            gst_buffer_unmap(out, &outMap);
        }
    }
    gst_sample_unref(sample);

    if (jpeg.empty()) return core::internalError("the JPEG encoder produced no data");
    return jpeg;
}

}  // namespace visora::media
