#include "media/ai/JpegDecoder.hpp"

#include <cstring>
#include <mutex>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "core/Log.hpp"
#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {
namespace {
constexpr GstClockTime kDecodeTimeout = 3 * GST_SECOND;
}

struct JpegDecoder::Impl {
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* appsink = nullptr;
    std::mutex mutex;
};

JpegDecoder::JpegDecoder() : m_impl(std::make_unique<Impl>()) {}
JpegDecoder::~JpegDecoder() { stop(); }

std::string JpegDecoder::launch() {
    LaunchPipeline pipeline;
    pipeline.chain()
        // decodebin, not jpegdec directly and not an "image/jpeg" capsfilter.
        //
        // A capsfilter of bare image/jpeg is refused — "filter caps do not
        // completely specify the output format" — because it has no width,
        // height or framerate, and nothing knows those until the bytes are
        // read. jpegdec on its own then gets no caps at all and reports "no
        // valid frames decoded". decodebin types the stream itself and sets
        // both, which is exactly the job it exists for.
        //
        // It is also what lets this accept a PNG a caller sends by mistake
        // rather than failing on a technicality.
        .add(ElementSpec("appsrc").named("src").set("format", "bytes"))
        .add("decodebin")
        .add("videoconvert")
        .caps("video/x-raw,format=RGB")
        .add(ElementSpec("appsink").named("out").set("sync", false).set("max-buffers", 1));
    return pipeline.toLaunch(/*wrapped=*/false);
}

core::Status JpegDecoder::start() {
    stop();
    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(launch().c_str(), &error);
    if (!m_impl->pipeline || error) {
        const std::string message =
            error && error->message ? error->message : "could not build the JPEG decoder";
        if (error) g_error_free(error);
        stop();
        return core::internalError(message);
    }
    m_impl->appsrc = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "src");
    m_impl->appsink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "out");
    if (!m_impl->appsrc || !m_impl->appsink) {
        stop();
        return core::internalError("the JPEG decoder is missing elements");
    }
    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        stop();
        return core::hardwareFailure("could not start the JPEG decoder");
    }
    return {};
}

void JpegDecoder::stop() {
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
}

core::Result<core::OwnedImage> JpegDecoder::decode(const std::uint8_t* bytes,
                                                   std::size_t size) {
    if (!bytes || size == 0) return core::invalidArgument("the image is empty");

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->pipeline) return core::unsupported("the JPEG decoder is not running");

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return core::internalError("could not map the input buffer");
    }
    std::memcpy(map.data, bytes, size);
    gst_buffer_unmap(buffer, &map);

    if (gst_app_src_push_buffer(GST_APP_SRC(m_impl->appsrc), buffer) != GST_FLOW_OK) {
        return core::internalError("the JPEG decoder rejected the image");
    }

    GstSample* sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(m_impl->appsink), kDecodeTimeout);
    if (!sample) return core::invalidArgument("that is not a JPEG this build can decode");

    core::OwnedImage image;
    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* decoded = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (caps && decoded && gst_video_info_from_caps(&info, caps)) {
        GstVideoFrame frame;
        if (gst_video_frame_map(&frame, &info, decoded, GST_MAP_READ)) {
            const int width = GST_VIDEO_FRAME_WIDTH(&frame);
            const int height = GST_VIDEO_FRAME_HEIGHT(&frame);
            image.reset(core::PixelFormat::RGB888, core::Size{width, height});
            const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
            const auto* source =
                static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
            // Row by row: the decoder pads rows to its own alignment, and a
            // straight copy embeds that padding as image data.
            for (int row = 0; row < height; ++row) {
                std::memcpy(image.data() + static_cast<std::size_t>(row) * width * 3,
                            source + static_cast<std::size_t>(row) * stride,
                            static_cast<std::size_t>(width) * 3);
            }
            gst_video_frame_unmap(&frame);
        }
    }
    gst_sample_unref(sample);

    if (image.empty()) return core::internalError("the JPEG decoder produced no image");
    return image;
}

}  // namespace visora::media
