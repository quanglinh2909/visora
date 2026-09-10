// visora-probe — prints what this build can do on this machine.
//
// Run it first on any new board. It answers "is the NPU being used", "why is AI
// disabled", "which 2D engine got picked" without attaching a debugger or
// rebuilding with printf. The same report is served over HTTP by the API layer.
//
//   visora-probe            human-readable table
//   visora-probe --json     machine-readable, for CI and deployment checks
//   visora-probe --model X  what tensors that model file actually produces
//
// The --model mode exists because a model type decodes SHAPES, and the shapes
// an export really has are the one thing no amount of reading the code will
// tell you. A YOLOv8 segmentation export was refused on a board as "not a
// segmentation export" and the only way to find out what it was instead was to
// look. It runs one inference on a blank frame to get them.

#include <cstdio>
#include <cstring>
#include <string>

#include <vector>

#include "core/Image.hpp"
#include "hal/Capabilities.hpp"
#include "hal/InferenceBackend.hpp"
#include "media/gst/CodecProvider.hpp"

namespace {

// Runs one inference on a blank frame and prints what came back. The values are
// meaningless; the SHAPES are the point.
int describeModel(const std::string& path) {
    auto model = visora::hal::loadModel(visora::hal::ModelRef{path});
    if (!model) {
        std::fprintf(stderr, "cannot load %s: %s\n", path.c_str(),
                     model.error().message.c_str());
        return 1;
    }

    const visora::core::Size size = model.value()->inputSize();
    const visora::core::PixelFormat format = model.value()->inputFormat();
    std::printf("model   : %s\n", path.c_str());
    std::printf("input   : %dx%d %s\n", size.width, size.height,
                visora::core::toString(format));

    visora::core::OwnedImage blank(format, size);
    blank.fill(114);
    auto tensors = model.value()->run(blank.view());
    if (!tensors) {
        std::fprintf(stderr, "inference failed: %s\n", tensors.error().message.c_str());
        return 1;
    }

    std::printf("outputs : %zu\n", tensors.value().outputs.size());
    std::size_t index = 0;
    for (const visora::hal::Tensor& tensor : tensors.value().outputs) {
        std::printf("  [%2zu] %-24s %-8s [", index++, tensor.name.c_str(),
                    visora::hal::toString(tensor.type));
        for (std::size_t d = 0; d < tensor.shape.size(); ++d) {
            std::printf("%s%d", d ? ", " : "", tensor.shape[d]);
        }
        std::printf("]");
        if (tensor.quant.quantised()) {
            std::printf("  scale=%g zp=%d", tensor.quant.scale, tensor.quant.zeroPoint);
        }
        std::printf("\n");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool json = false;
    std::string modelPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            modelPath = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "usage: visora-probe [--json]\n"
                "\n"
                "Environment:\n"
                "  VISORA_IMAGE_BACKEND=<id>       force an image-ops backend\n"
                "  VISORA_INFERENCE_BACKEND=<id>   force an inference backend\n"
                "  VISORA_LOG_LEVEL=trace|debug|info|warn|error|off\n"
                "  VISORA_LOG_CATEGORIES=hal,cpu-imageops\n");
            return 0;
        }
    }

    if (!modelPath.empty()) return describeModel(modelPath);

    visora::hal::Capabilities caps = visora::hal::detectCapabilities();

    // The application joins the layers. hal cannot ask media what it found —
    // that would invert the dependency order the whole build enforces.
    for (auto& row : visora::media::codecBackendStatus()) {
        caps.backends.push_back(std::move(row));
    }
    const std::string out = json ? visora::hal::toJson(caps) : visora::hal::toText(caps);
    std::fwrite(out.data(), 1, out.size(), stdout);
    if (json) std::fputc('\n', stdout);

    // Non-zero when nothing can run, so a deployment script can gate on it.
    return caps.imageOpsSelected.empty() ? 1 : 0;
}
