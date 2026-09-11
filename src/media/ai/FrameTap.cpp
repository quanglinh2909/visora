#include "media/ai/FrameTap.hpp"

#include <algorithm>
#include <vector>

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

const char* gstFormatName(core::PixelFormat format) {
    switch (format) {
        case core::PixelFormat::NV12:   return "NV12";
        case core::PixelFormat::RGB888: return "RGB";
        case core::PixelFormat::BGR888: return "BGR";
        case core::PixelFormat::GRAY8:  return "GRAY8";
        case core::PixelFormat::Unknown: break;
    }
    return "NV12";
}

}  // namespace

struct FrameTap::Impl {
    FrameTap* owner = nullptr;
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* appsink = nullptr;
    BusWatcher bus;
    std::uint64_t sourceSinkId = 0;

    std::mutex pushMutex;
    bool enabled = false;
    bool capsSet = false;
    std::atomic<bool> running{false};

    mutable std::mutex sinkMutex;
    std::map<std::uint64_t, Sink> sinks;
    std::uint64_t nextSinkId = 1;

    void push(GstBuffer* buffer, GstCaps* caps);
    void deliver(GstSample* sample);
};

void FrameTap::Impl::push(GstBuffer* buffer, GstCaps* caps) {
    std::lock_guard<std::mutex> lock(pushMutex);
    if (!enabled || !appsrc) return;
    if (!capsSet && caps) {
        gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
        capsSet = true;
    }
    GstBuffer* out = gst_buffer_make_writable(gst_buffer_ref(buffer));
    // Same reason as everywhere else: the buffer carries the SOURCE pipeline's
    // clock, and do-timestamp only stamps one that has none.
    GST_BUFFER_PTS(out) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(out) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(out) = GST_CLOCK_TIME_NONE;
    gst_app_src_push_buffer(GST_APP_SRC(appsrc), out);
}

void FrameTap::Impl::deliver(GstSample* sample) {
    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!caps || !buffer) return;

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) return;

    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) return;

    core::ImageView view;
    view.format = core::PixelFormat::NV12;
    view.size = core::Size{GST_VIDEO_FRAME_WIDTH(&frame), GST_VIDEO_FRAME_HEIGHT(&frame)};
    view.data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    // Strides from the FRAME, not computed from the width. A hardware decoder
    // pads rows to its own alignment, and reading such a frame as if it were
    // packed produces a picture that leans progressively further sideways.
    const int planes = std::min<int>(GST_VIDEO_FRAME_N_PLANES(&frame), core::kMaxPlanes);
    for (int i = 0; i < planes; ++i) {
        view.planes[i].stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, i);
        view.planes[i].offset =
            static_cast<std::size_t>(GST_VIDEO_FRAME_PLANE_OFFSET(&frame, i));
    }

    const std::int64_t tsUs =
        GST_BUFFER_PTS_IS_VALID(buffer)
            ? static_cast<std::int64_t>(GST_BUFFER_PTS(buffer) / GST_USECOND)
            : 0;

    std::vector<Sink> targets;
    {
        std::lock_guard<std::mutex> lock(sinkMutex);
        targets.reserve(sinks.size());
        for (const auto& [id, sink] : sinks) targets.push_back(sink);
    }
    for (const Sink& sink : targets) sink(view, tsUs);

    gst_video_frame_unmap(&frame);
}

namespace {

GstFlowReturn onNewSample(GstAppSink* appsink, gpointer user) {
    auto* impl = static_cast<FrameTap::Impl*>(user);
    GstSample* sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_OK;
    impl->deliver(sample);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

}  // namespace

FrameTap::FrameTap(std::string cameraId, std::shared_ptr<EncodedSource> source,
                   FrameTapOptions options)
    : m_cameraId(std::move(cameraId)),
      m_source(std::move(source)),
      m_options(options),
      m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
}

FrameTap::~FrameTap() { stop(); }

std::string FrameTap::launchFor(Codec codec, const FrameTapOptions& options) {
    if (codec == Codec::Unknown) return {};

    // The decoder comes from the codec providers, so this is MPP on a Rockchip
    // board, NVDEC on an NVIDIA machine and libav where there is nothing else.
    const auto decoder = resolveDecoder(codec);
    if (!decoder) return {};

    LaunchPipeline pipeline;
    LaunchChain& chain = pipeline.chain();
    chain.add(ElementSpec("appsrc")
                  .named("src")
                  .set("is-live", true)
                  .set("format", "time")
                  .set("do-timestamp", true)
                  .set("max-bytes", 0)
                  .set("block", false))
        .caps(parsedCaps(codec))
        .add(ElementSpec(parser(codec)).set("config-interval", -1))
        .add(decoder->spec);

    if (options.maxFps > 0) {
        // drop-only: never DUPLICATE a frame to reach a rate. Analysing the
        // same picture twice is pure waste, and it makes a detector look like
        // it found something twice.
        chain.add(ElementSpec("videorate").set("drop-only", true))
            .caps("video/x-raw,framerate=" + std::to_string(options.maxFps) + "/1");
    }

    // videoconvert only if the decoder cannot already produce the format. It is
    // a passthrough when it can, and the most expensive element in the pipeline
    // when it cannot — measured at 15% of a core per 1080p camera.
    chain.add("videoconvert")
        .caps(std::string("video/x-raw,format=") + gstFormatName(options.format))
        .add(ElementSpec("appsink")
                 .named("out")
                 .set("sync", false)
                 // ONE buffer, and drop. A consumer slower than the camera must
                 // skip frames rather than fall behind: a frame from four
                 // seconds ago is worth nothing to a detector.
                 .set("max-buffers", 1)
                 .set("drop", true));

    return pipeline.toLaunch(/*wrapped=*/false);
}

core::Status FrameTap::start() {
    stop();
    if (!m_source) return core::invalidArgument("the frame tap needs a source");

    const std::string launch = launchFor(m_source->codec(), m_options);
    if (launch.empty()) {
        return core::unsupported("no decoder for codec " +
                                 std::string(toString(m_source->codec())));
    }
    VS_DEBUG(kCategory) << m_cameraId << ": " << launch;

    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(launch.c_str(), &error);
    if (!m_impl->pipeline || error) {
        const std::string message =
            error && error->message ? error->message : "could not build the decode pipeline";
        if (error) g_error_free(error);
        stop();
        return core::internalError(message);
    }

    m_impl->appsrc = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "src");
    m_impl->appsink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "out");
    if (!m_impl->appsrc || !m_impl->appsink) {
        stop();
        return core::internalError("the decode pipeline is missing elements");
    }

    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = &onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_impl->appsink), &callbacks, m_impl.get(),
                               nullptr);

    m_impl->bus.start(m_impl->pipeline, m_cameraId, [this](GstMessage* message) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            VS_WARN(kCategory) << m_cameraId << ": decode error: " << errorTextOf(message);
        }
    });

    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        stop();
        return core::hardwareFailure("could not start decoding " + m_cameraId);
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->pushMutex);
        m_impl->enabled = true;
    }
    // No GOP priming. The analyser samples a few frames a second on purpose,
    // and replaying two seconds of cached video into it means a decode burst and
    // NPU work to produce detections for a moment that has already passed. It
    // starts at the next keyframe, as it always did.
    m_impl->sourceSinkId = m_source->addSink(
        [impl = m_impl.get()](GstBuffer* buffer, GstCaps* caps) { impl->push(buffer, caps); },
        SinkOptions{/*primeFromGop=*/false});
    m_impl->running.store(true);

    VS_INFO(kCategory) << m_cameraId << ": decoding for AI ("
                       << (m_options.maxFps > 0 ? std::to_string(m_options.maxFps) + " fps"
                                                : std::string("every frame"))
                       << ')';
    return {};
}

void FrameTap::stop() {
    {
        std::lock_guard<std::mutex> lock(m_impl->pushMutex);
        m_impl->enabled = false;
        m_impl->capsSet = false;
    }
    if (m_source && m_impl->sourceSinkId != 0) {
        m_source->removeSink(m_impl->sourceSinkId);
        m_impl->sourceSinkId = 0;
    }
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
    if (m_impl->running.exchange(false)) {
        VS_INFO(kCategory) << m_cameraId << ": stopped decoding";
    }
}

std::uint64_t FrameTap::addSink(Sink sink) {
    std::lock_guard<std::mutex> lock(m_impl->sinkMutex);
    const std::uint64_t id = m_impl->nextSinkId++;
    m_impl->sinks.emplace(id, std::move(sink));
    return id;
}

void FrameTap::removeSink(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(m_impl->sinkMutex);
    m_impl->sinks.erase(id);
}

std::size_t FrameTap::sinkCount() const {
    std::lock_guard<std::mutex> lock(m_impl->sinkMutex);
    return m_impl->sinks.size();
}

bool FrameTap::running() const { return m_impl->running.load(); }

}  // namespace visora::media
