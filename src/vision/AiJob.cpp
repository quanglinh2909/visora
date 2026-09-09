#include "vision/AiJob.hpp"

namespace visora::vision {
namespace {

template <class T>
bool assign(const std::optional<T>& from, T& to) {
    if (!from.has_value() || *from == to) return false;
    to = *from;
    return true;
}

bool sameStages(const std::vector<AiStage>& a, const std::vector<AiStage>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].modelType != b[i].modelType || a[i].modelPath != b[i].modelPath ||
            a[i].parent != b[i].parent || a[i].transform != b[i].transform ||
            a[i].confidence != b[i].confidence || a[i].inputClasses != b[i].inputClasses ||
            a[i].classFilter != b[i].classFilter) {
            return false;
        }
    }
    return true;
}

}  // namespace

AiJobDiff apply(const AiJobChanges& changes, AiJob& job) {
    AiJobDiff diff;

    assign(changes.name, job.name);
    if (assign(changes.cameraId, job.cameraId)) diff.pipelineChanged = true;
    if (assign(changes.enabled, job.enabled)) diff.enabledChanged = true;
    // A frame-rate ceiling is applied by the worker each frame, so changing it
    // must not reload a model — which on an NPU takes long enough to notice.
    assign(changes.maxFps, job.maxFps);

    if (changes.stages.has_value() && !sameStages(*changes.stages, job.stages)) {
        job.stages = *changes.stages;
        diff.pipelineChanged = true;
    }
    return diff;
}

}  // namespace visora::vision
