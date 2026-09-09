#pragma once

// Neural-network execution, abstracted at the TENSOR level rather than the
// model level.
//
// Abstracting at the model level ("give me a YOLOv8 detector") would force every
// backend to reimplement the postprocessing maths. Abstracting at the tensor
// level means the existing int8 decode, NMS, pose and OCR postprocessing is
// written once and runs unchanged whether the tensors came off an NPU or out of
// ONNX Runtime — which is the only reason porting those files is tractable at
// all.
//
// Quantisation parameters travel with the tensor for the same reason: an int8
// NPU output is meaningless without its zero point and scale, and the
// postprocessor is the one that needs them.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/Image.hpp"
#include "core/Result.hpp"
#include "hal/Registry.hpp"

namespace visora::hal {

enum class TensorType { Unknown, Int8, UInt8, Float32 };

const char* toString(TensorType type);

struct Quantisation {
    // value = (raw - zeroPoint) * scale. `scale == 0` means the tensor is not
    // quantised and `raw` is already the value.
    float scale = 0.0f;
    std::int32_t zeroPoint = 0;

    bool quantised() const { return scale != 0.0f; }
};

struct Tensor {
    std::string name;
    TensorType type = TensorType::Unknown;
    std::vector<int> shape;      // e.g. {1, 255, 80, 80}
    Quantisation quant;
    const void* data = nullptr;  // owned by the TensorSet
    std::size_t byteCount = 0;

    std::size_t elementCount() const;
};

// The outputs of one inference. Owns its buffers so the caller may keep it
// after the backend has moved on to the next frame.
class TensorSet {
public:
    std::vector<Tensor> outputs;
    std::vector<std::vector<std::uint8_t>> storage;

    const Tensor* find(std::string_view name) const;
};

// What a model needs to be loaded: a path plus whatever the backend requires to
// decide it can handle it. Backends match on the file extension they support
// (.rknn, .onnx, .engine) so the same job definition works everywhere provided
// the matching artefact exists beside it.
struct ModelRef {
    std::string path;
};

class Model {
public:
    virtual ~Model() = default;

    virtual core::Size inputSize() const = 0;
    virtual core::PixelFormat inputFormat() const = 0;

    // Runs one inference. `input` must already be at inputSize()/inputFormat();
    // fitting it there is the ImageOps layer's job, not the backend's.
    virtual core::Result<TensorSet> run(const core::ImageView& input) = 0;
};

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    virtual std::string_view id() const = 0;

    // Whether this backend can load that artefact at all — checked before
    // loading so the caller can fall through to another backend.
    virtual bool handles(const ModelRef& model) const = 0;

    virtual core::Result<std::unique_ptr<Model>> load(const ModelRef& model) = 0;
};

Registry<InferenceBackend>& inferenceRegistry();

// Selects once and caches. `VISORA_INFERENCE_BACKEND` forces a specific id.
// Returns NotFound when nothing is available — the caller reports AI as
// disabled with that reason rather than crashing.
core::Result<InferenceBackend*> inference();

// Loads a model on whichever available backend can handle that artefact.
//
// Prefer this to inference(): the selected backend is not necessarily the one
// that understands a given file. A board with an NPU still needs ONNX Runtime
// for a .onnx, and a machine with both should use each for what it is for. Same
// reasoning as resolveDecoder in the codec layer — asking only the best backend
// fails on a machine that can do the work perfectly well with the next one.
core::Result<std::unique_ptr<Model>> loadModel(const ModelRef& model);

// Every available backend, best first. Exposed for the capability report.
std::vector<InferenceBackend*> availableInferenceBackends();

}  // namespace visora::hal
