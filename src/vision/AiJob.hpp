#pragma once

// What to run on a camera, as the business defines it.
//
// A job is a TREE of stages, not a fixed pair of models. Stage zero runs on the
// whole frame; every later stage names a parent, and for each detection the
// parent kept it crops that detection out of the frame and runs its own model
// on it, hanging what it finds under the parent.
//
//     stage 0: yolov8        (car, motorcycle, truck, plate)
//       ├─ stage 1: ocr        inputClasses = {plate}
//       └─ stage 2: classifier inputClasses = {car, motorcycle, truck}
//
// Two stages sharing a parent run over the same boxes; stages chained one after
// another form a pipeline. The same data structure covers both, so a new
// problem is one more element in an array — not a new code path, which is what
// "model 1 and optional model 2" made every previous problem into.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace visora::vision {

struct AiStage {
    // A registered model type id — see vision/ModelType.hpp.
    std::string modelType;
    // Where the artefact lives. The backend is chosen by what it is: a .rknn
    // goes to the NPU, a .onnx to ONNX Runtime, and the same job definition
    // works on both machines provided the matching file is there.
    std::string modelPath;

    // Which of the PARENT's classes this stage runs on. Empty means all of
    // them. This is the input filter, not the output one.
    std::vector<int> inputClasses;
    // Which of this stage's OWN detections to keep. Empty means all.
    std::vector<int> classFilter;

    float confidence = 0.25f;

    // The stage this one crops from. -1 means the whole frame, which stage zero
    // must be and no other stage may be.
    int parent = -1;

    // A registered transform id; empty is the plain crop.
    std::string transform;
};

struct AiJob {
    std::string id;
    std::string name;
    std::string cameraId;
    bool enabled = true;
    // 0 means "as fast as the frames arrive". Anything else is a ceiling: an
    // NPU shared between twelve cameras is better spent on ten frames a second
    // each than on starving one of them.
    int maxFps = 0;
    std::vector<AiStage> stages;
};

// What a caller may set. Optionals distinguish "leave it alone" from "set it to
// the default", the same way CameraChanges does.
struct AiJobChanges {
    std::optional<std::string> name;
    std::optional<std::string> cameraId;
    std::optional<bool> enabled;
    std::optional<int> maxFps;
    std::optional<std::vector<AiStage>> stages;
};

struct AiJobDiff {
    bool pipelineChanged = false;  // needs the worker rebuilt
    bool enabledChanged = false;
    bool cosmeticOnly() const { return !pipelineChanged && !enabledChanged; }
};

AiJobDiff apply(const AiJobChanges& changes, AiJob& job);

}  // namespace visora::vision
