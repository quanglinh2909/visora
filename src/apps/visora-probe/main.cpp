// visora-probe — prints what this build can do on this machine.
//
// Run it first on any new board. It answers "is the NPU being used", "why is AI
// disabled", "which 2D engine got picked" without attaching a debugger or
// rebuilding with printf. The same report is served over HTTP by the API layer.
//
//   visora-probe            human-readable table
//   visora-probe --json     machine-readable, for CI and deployment checks

#include <cstdio>
#include <cstring>
#include <string>

#include "hal/Capabilities.hpp"
#include "media/gst/CodecProvider.hpp"

int main(int argc, char** argv) {
    bool json = false;
    for (int i = 1; i < argc; ++i) {
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
