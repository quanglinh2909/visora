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

}  // namespace visora::media
