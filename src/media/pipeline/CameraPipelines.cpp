#include "media/pipeline/CameraPipelines.hpp"

#include <algorithm>
#include <string>

#include "media/gst/ElementSpec.hpp"

namespace visora::media {

std::string mountPath(const std::string& cameraId) { return "/cameras/" + cameraId; }

std::string restreamLaunch(const CameraSource& camera, const RestreamOptions& options) {
    if (camera.codec == Codec::Unknown) return {};

    const int latency = std::max(options.latencyMs, RestreamOptions::kMinimumLatencyMs);

    LaunchPipeline pipeline;
    pipeline.chain()
        .add(ElementSpec("rtspsrc")
                 .named("src")
                 .setQuoted("location", camera.rtspUrl)
                 .set("latency", latency)
                 .set("protocols", "tcp"))
        .caps(std::string("application/x-rtp,media=video,encoding-name=") +
              encodingName(camera.codec))
        .add(depayloader(camera.codec))
        // config-interval=-1 on the parser re-sends SPS/PPS with every
        // keyframe, so a viewer joining mid-stream can decode without waiting
        // for the camera to send parameter sets again.
        .add(ElementSpec(parser(camera.codec)).set("config-interval", -1))
        .add(ElementSpec(payloader(camera.codec))
                 .named("pay0")
                 .set("config-interval", 1)
                 .set("pt", 96));

    return pipeline.toLaunch(/*wrapped=*/true);
}

std::string restreamFromSourceLaunch(Codec codec) {
    if (codec == Codec::Unknown) return {};
    const std::string payloaderName = payloader(codec);
    if (payloaderName.empty()) return {};

    LaunchPipeline pipeline;
    pipeline.chain()
        // do-timestamp=true because the buffers arrive stripped of the source
        // pipeline's clock — the shared source's timestamps belong to a
        // different clock with a different base, and a payloader fed those
        // produces RTP timestamps no client can follow.
        .add(ElementSpec("appsrc")
                 .named(kRestreamAppSrcName)
                 .set("is-live", true)
                 .set("format", "time")
                 .set("do-timestamp", true)
                 .set("max-bytes", 0)
                 .set("block", false))
        .caps(parsedCaps(codec))
        .add("queue")
        // config-interval=-1 re-sends SPS/PPS with every keyframe, so a viewer
        // joining mid-stream decodes without waiting for the camera to send
        // parameter sets again.
        .add(ElementSpec(parser(codec)).set("config-interval", -1))
        .add(ElementSpec(payloaderName).named("pay0").set("config-interval", 1).set("pt", 96));

    return pipeline.toLaunch(/*wrapped=*/true);
}

}  // namespace visora::media
