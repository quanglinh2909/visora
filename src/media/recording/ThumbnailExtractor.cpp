#include "media/recording/ThumbnailExtractor.hpp"

#include <algorithm>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "core/Log.hpp"
#include "media/gst/CodecProvider.hpp"
#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "thumbnail";

// How many frames to look at before giving up. Enough to cross the run-back
// plus a refresh cycle; the size rule below normally stops far sooner.
constexpr int kMaxFrames = 64;

class GstThumbnailExtractor : public ThumbnailExtractor {
public:
    core::Result<std::vector<std::uint8_t>> extract(const std::string& path, Codec codec,
                                                    std::int64_t offsetMs,
                                                    const ThumbnailOptions& options) override {
        const std::string launch = thumbnailLaunch(path, codec, options);
        if (launch.empty()) {
            return core::unsupported("cannot extract a frame from codec " +
                                     std::string(toString(codec)));
        }
        VS_DEBUG(kCategory) << launch;

        GError* error = nullptr;
        GstElement* pipeline = gst_parse_launch(launch.c_str(), &error);
        if (!pipeline || error) {
            const std::string message =
                error && error->message ? error->message : "could not build the pipeline";
            if (error) g_error_free(error);
            if (pipeline) gst_object_unref(pipeline);
            return core::internalError(message);
        }

        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "out");
        if (!sink) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return core::internalError("thumbnail pipeline has no sink");
        }

        auto result = run(pipeline, sink, offsetMs);

        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return result;
    }

private:
    static core::Result<std::vector<std::uint8_t>> run(GstElement* pipeline, GstElement* sink,
                                                       std::int64_t offsetMs) {
        // PAUSED and wait for preroll first: a seek on a pipeline that has no
        // state yet is silently ignored, and the thumbnail then comes from the
        // start of the file no matter what was asked for.
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        GstState state = GST_STATE_NULL;
        if (gst_element_get_state(pipeline, &state, nullptr, 3 * GST_SECOND) ==
            GST_STATE_CHANGE_FAILURE) {
            return core::internalError("could not open the recording");
        }

        const std::int64_t seekMs =
            offsetMs > kThumbnailRunbackMs ? offsetMs - kThumbnailRunbackMs : 0;
        if (seekMs > 0) {
            // SNAP_BEFORE|KEY_UNIT: land on the keyframe at or before the
            // target. Landing after it would start decoding mid-GOP.
            gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
                                    static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                              GST_SEEK_FLAG_KEY_UNIT |
                                                              GST_SEEK_FLAG_SNAP_BEFORE),
                                    seekMs * GST_MSECOND);
        }
        // seekMs == 0 means the target is near the start; a segment always
        // opens on a clean keyframe, so decoding from the beginning is right.

        // PLAYING rather than reading the preroll frame: after a flushing seek
        // the preroll is routinely a filler frame.
        gst_element_set_state(pipeline, GST_STATE_PLAYING);

        // Keep the LARGEST JPEG, and stop once the size drops.
        //
        // Not a fixed byte threshold. With gradual refresh the decoder emits a
        // run of increasingly-complete grey frames, and those are not small —
        // several kilobytes each — so any absolute threshold picks one of them.
        // What holds regardless of width or scene is the SHAPE: size climbs
        // monotonically through the grey frames, the first complete frame is a
        // maximum several times larger, and the P-frame right after it is much
        // smaller. On an ordinary stream the first frame is already the largest
        // and this stops at the third.
        std::vector<std::uint8_t> best;
        for (int i = 0; i < kMaxFrames; ++i) {
            GstSample* sample =
                gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
            if (!sample) break;

            std::vector<std::uint8_t> frame;
            if (GstBuffer* buffer = gst_sample_get_buffer(sample)) {
                GstMapInfo info;
                if (gst_buffer_map(buffer, &info, GST_MAP_READ)) {
                    frame.assign(info.data, info.data + info.size);
                    gst_buffer_unmap(buffer, &info);
                }
            }
            gst_sample_unref(sample);

            if (frame.size() > best.size()) {
                best = std::move(frame);
                continue;
            }
            // Smaller than the best so far: the complete frame has been passed.
            if (!best.empty()) break;
        }

        if (best.empty()) return core::internalError("no frame could be decoded");
        return best;
    }
};

}  // namespace

std::string thumbnailLaunch(const std::string& path, Codec codec,
                            const ThumbnailOptions& options) {
    if (codec == Codec::Unknown || path.empty()) return {};

    const auto jpeg = resolveJpegEncoder(options.quality);
    if (!jpeg) return {};

    // Software decoding on purpose, and not decodebin.
    //
    // This decodes a couple of dozen frames on a seek into the middle of a
    // file, which is exactly the case hardware decoders handle worst: several
    // return grey frames for orphaned P-frames rather than dropping them. The
    // cost is one frame's worth of CPU on a rare request.
    const char* decoder = codec == Codec::H265 ? "avdec_h265" : "avdec_h264";

    LaunchPipeline pipeline;
    pipeline.chain()
        .add(ElementSpec("filesrc").setQuoted("location", path))
        .add(ElementSpec("tsdemux").named("d"))
        // config-interval=-1 re-inserts the parameter sets before every
        // keyframe. Without it, a seek into the middle of a file leaves the
        // decoder without the parameters it needs and it emits grey frames
        // until it happens across them again.
        .add(ElementSpec(parser(codec)).set("config-interval", -1))
        // output-corrupt=false is the other half of the grey-frame fix. After a
        // seek, the decoder produces the orphaned P-frames whose references
        // were flushed; by default it pushes them out marked corrupt, which can
        // be forty grey frames. False drops them, so the first frame out is a
        // real one.
        .add(ElementSpec(decoder).set("output-corrupt", false))
        .add("videoconvert")
        .add("videoscale")
        // Width only, with square pixels: videoscale computes the height and
        // the aspect ratio is preserved without knowing the source size.
        .caps("video/x-raw,width=" + std::to_string(std::max(16, options.width)) +
              ",pixel-aspect-ratio=1/1")
        .add(jpeg->spec)
        .add(ElementSpec("appsink")
                 .named("out")
                 .set("sync", false)
                 .set("max-buffers", 4)
                 .set("drop", false));

    return pipeline.toLaunch(/*wrapped=*/false);
}

std::shared_ptr<ThumbnailExtractor> makeGstThumbnailExtractor() {
    return std::make_shared<GstThumbnailExtractor>();
}

}  // namespace visora::media
