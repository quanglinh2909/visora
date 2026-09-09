#pragma once

// A KIND of model — what to do with the tensors, not how to run them.
//
// The split that makes this tractable: hal::InferenceBackend runs tensors on
// whatever hardware exists, and a ModelType turns those tensors into
// detections. So YOLOv8's int8 decode and NMS are written ONCE and work
// unchanged on an NPU, on ONNX Runtime, on CUDA — the postprocessing does not
// know or care.
//
// Registered rather than listed. The predecessor had an if/else chain naming
// every concrete class, plus a parallel vector of type names that had to be
// kept in step by hand; adding a model type meant editing both and the REST
// validation that read them. Here a new type is a new file with a
// core::Register at the bottom, and it appears in GET /ai-model-types by
// itself.
//
// A model type is STAGE-AGNOSTIC. A job's first stage runs on the whole frame
// and later stages run on crops of what earlier ones found, and any type may be
// used in either position: a detector as stage two is how a plate detector runs
// on a car crop. A type implements only the roles that make sense for it, and
// the defaults make the others produce nothing rather than crash.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/Image.hpp"
#include "core/Registry.hpp"
#include "core/Result.hpp"
#include "hal/InferenceBackend.hpp"
#include "vision/Detection.hpp"

namespace visora::vision {

// How a frame or crop is fitted into the model's input.
//
// Not cosmetic. Measured on the predecessor with six real number plates:
// stretching to fill read 0 of 6 correctly, letterboxing read 5 of 6. A
// detector wants no distortion; some OCR families want the opposite.
enum class FramePrep {
    // Preserve the aspect ratio and pad. What detectors are trained on.
    Letterbox,
    // Fill the input, distorting. What some recognisers expect.
    Stretch,
    // Preserve the aspect ratio, match the height, pad or crop the width. The
    // PP-OCR recognition family.
    FitHeight,
};

const char* toString(FramePrep prep);

// What a model type needs to interpret its own outputs.
struct ModelContext {
    // Size of the image that was actually fed to the model.
    core::Size inputSize;
    // Where the content sits inside that input, after letterboxing. Detections
    // are mapped back through this, so a padded model input still yields boxes
    // in source coordinates.
    core::Rect contentRect;
    // Size of the image the content was taken from.
    core::Size sourceSize;

    float confidence = 0.25f;
    float nmsThreshold = 0.45f;
    // Class ids to keep. Empty means keep everything.
    std::vector<int> classFilter;
};

class ModelType {
public:
    virtual ~ModelType() = default;

    virtual std::string_view id() const = 0;
    virtual std::string_view label() const = 0;
    virtual std::string_view description() const = 0;

    virtual FramePrep framePrep() const { return FramePrep::Letterbox; }

    // Whether a crop for this model should be tight to the box rather than
    // padded with context. DETR-style detectors are trained on a plain resize
    // of the tight object; YOLO wants context around it.
    virtual bool prefersTightCrop() const { return false; }

    // STAGE-ONE role: tensors from a whole frame become detections.
    // The default says "this type is not a detector".
    virtual core::Result<std::vector<Detection>> decode(const hal::TensorSet& /*tensors*/,
                                                        const ModelContext& /*context*/) const {
        return std::vector<Detection>{};
    }

    // STAGE-TWO role: tensors from a crop enrich the detection it came from.
    //
    // The default runs decode() and attaches what it found as children, so
    // every detector type works as a later stage unchanged. A type that
    // enriches differently — a face embedding, a text string — overrides this.
    virtual core::Status enrich(const hal::TensorSet& tensors, const ModelContext& context,
                                Detection& parent) const;

    // A human-readable name for a class id, when the type carries its own
    // labels (an OCR dictionary). Empty means the consumer only gets the id.
    virtual std::string labelFor(int /*classId*/) const { return {}; }
};

core::Registry<ModelType>& modelTypeRegistry();

// Every registered type, for GET /ai-model-types and for validating a job.
std::vector<ModelType*> modelTypes();
ModelType* modelType(std::string_view id);

}  // namespace visora::vision
