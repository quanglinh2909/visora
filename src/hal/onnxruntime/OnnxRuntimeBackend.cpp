// ONNX Runtime inference.
//
// The portable backend, and the one that makes this product run on hardware
// nobody has written a backend for: ONNX Runtime has execution providers for
// CUDA, TensorRT, OpenVINO, DirectML, CoreML and more, and they are selected
// here by asking which ones the installed build actually has. So an NVIDIA
// workstation gets CUDA, an Intel box gets OpenVINO and a bare server gets the
// CPU — from one file, with no build variants.
//
// The postprocessing is untouched by any of that: this produces a TensorSet and
// vision/ decodes it, exactly as it does for the NPU.

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "core/Log.hpp"
#include "hal/InferenceBackend.hpp"

namespace visora::hal {
namespace {

constexpr const char* kCategory = "hal";

TensorType typeOf(ONNXTensorElementDataType type) {
    switch (type) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:  return TensorType::Float32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:   return TensorType::Int8;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:  return TensorType::UInt8;
        default: break;
    }
    return TensorType::Unknown;
}

// The environment is process-wide and expensive: it owns the thread pools and
// the logging. One for the life of the program, shared by every model.
Ort::Env& environment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "visora");
    return env;
}

// The providers this build actually has, best first.
//
// Asked rather than assumed. The same code then uses CUDA on a machine with a
// GPU build installed and falls to the CPU on one without, instead of failing
// to start because a provider it was compiled to expect is missing.
const std::vector<std::string>& availableProviders() {
    static const std::vector<std::string> providers = [] {
        std::vector<std::string> all = Ort::GetAvailableProviders();
        // Fastest first. TensorRT beats plain CUDA where it is present because
        // it compiles the graph; OpenVINO is Intel's equivalent.
        static const char* order[] = {"TensorrtExecutionProvider", "CUDAExecutionProvider",
                                      "OpenVINOExecutionProvider", "ROCMExecutionProvider",
                                      "CoreMLExecutionProvider", "DmlExecutionProvider"};
        std::vector<std::string> ranked;
        for (const char* name : order) {
            if (std::find(all.begin(), all.end(), name) != all.end()) ranked.emplace_back(name);
        }
        return ranked;
    }();
    return providers;
}

std::string describeProviders() {
    const auto& providers = availableProviders();
    if (providers.empty()) return "CPU";
    std::string text;
    for (const std::string& provider : providers) {
        if (!text.empty()) text += ", ";
        // "CUDAExecutionProvider" -> "CUDA"
        const auto at = provider.find("ExecutionProvider");
        text += at == std::string::npos ? provider : provider.substr(0, at);
    }
    return text + ", CPU";
}

class OnnxModel final : public Model {
public:
    OnnxModel(std::unique_ptr<Ort::Session> session, core::Size inputSize,
              core::PixelFormat inputFormat, bool nchw)
        : m_session(std::move(session)),
          m_inputSize(inputSize),
          m_inputFormat(inputFormat),
          m_nchw(nchw) {
        Ort::AllocatorWithDefaultOptions allocator;
        for (std::size_t i = 0; i < m_session->GetInputCount(); ++i) {
            m_inputNames.push_back(m_session->GetInputNameAllocated(i, allocator).get());
        }
        for (std::size_t i = 0; i < m_session->GetOutputCount(); ++i) {
            m_outputNames.push_back(m_session->GetOutputNameAllocated(i, allocator).get());
        }
    }

    core::Size inputSize() const override { return m_inputSize; }
    core::PixelFormat inputFormat() const override { return m_inputFormat; }

    core::Result<TensorSet> run(const core::ImageView& input) override {
        if (!input.valid() || !input.hasCpu()) {
            return core::invalidArgument("ONNX Runtime needs a CPU-readable image");
        }
        if (input.size.width != m_inputSize.width || input.size.height != m_inputSize.height) {
            return core::invalidArgument("the image is not the model's input size");
        }

        // RGB bytes to normalised float, and to NCHW if that is what the model
        // wants. This is the conversion an NPU does in hardware, which is why
        // the ONNX path costs more per frame on the same machine.
        const int width = m_inputSize.width;
        const int height = m_inputSize.height;
        const int stride = input.planes[0].stride > 0 ? input.planes[0].stride : width * 3;
        m_input.resize(static_cast<std::size_t>(width) * height * 3);

        for (int y = 0; y < height; ++y) {
            const std::uint8_t* row = input.data + input.planes[0].offset +
                                      static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float value = static_cast<float>(row[x * 3 + c]) / 255.0f;
                    const std::size_t at =
                        m_nchw ? static_cast<std::size_t>(c) * width * height +
                                     static_cast<std::size_t>(y) * width + x
                               : (static_cast<std::size_t>(y) * width + x) * 3 + c;
                    m_input[at] = value;
                }
            }
        }

        const std::array<std::int64_t, 4> shape =
            m_nchw ? std::array<std::int64_t, 4>{1, 3, height, width}
                   : std::array<std::int64_t, 4>{1, height, width, 3};

        Ort::MemoryInfo memory =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(
            memory, m_input.data(), m_input.size(), shape.data(), shape.size());

        std::vector<const char*> inputNames;
        for (const std::string& name : m_inputNames) inputNames.push_back(name.c_str());
        std::vector<const char*> outputNames;
        for (const std::string& name : m_outputNames) outputNames.push_back(name.c_str());

        std::vector<Ort::Value> outputs;
        try {
            outputs = m_session->Run(Ort::RunOptions{nullptr}, inputNames.data(), &tensor, 1,
                                     outputNames.data(), outputNames.size());
        } catch (const Ort::Exception& error) {
            return core::internalError(std::string("onnxruntime: ") + error.what());
        }

        TensorSet set;
        set.storage.reserve(outputs.size());
        set.outputs.reserve(outputs.size());
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const auto info = outputs[i].GetTensorTypeAndShapeInfo();
            Tensor out;
            out.name = m_outputNames[i];
            out.type = typeOf(info.GetElementType());
            for (const std::int64_t dimension : info.GetShape()) {
                out.shape.push_back(static_cast<int>(dimension));
            }
            // ONNX float outputs are not quantised, so scale stays 0 and the
            // decoder reads the values directly. The SAME decoder that reads an
            // int8 NPU tensor with its zero point and scale.
            const std::size_t bytes = info.GetElementCount() * elementSize(out.type);
            const auto* raw = static_cast<const std::uint8_t*>(outputs[i].GetTensorRawData());
            set.storage.emplace_back(raw, raw + bytes);
            out.data = set.storage.back().data();
            out.byteCount = bytes;
            set.outputs.push_back(std::move(out));
        }
        return set;
    }

private:
    static std::size_t elementSize(TensorType type) {
        switch (type) {
            case TensorType::Float32: return 4;
            case TensorType::Int8:
            case TensorType::UInt8:   return 1;
            case TensorType::Unknown: break;
        }
        return 1;
    }

    std::unique_ptr<Ort::Session> m_session;
    core::Size m_inputSize;
    core::PixelFormat m_inputFormat;
    bool m_nchw = true;
    std::vector<std::string> m_inputNames;
    std::vector<std::string> m_outputNames;
    std::vector<float> m_input;
};

class OnnxRuntimeBackend final : public InferenceBackend {
public:
    std::string_view id() const override { return "onnxruntime"; }

    bool handles(const ModelRef& model) const override {
        return model.path.size() > 5 &&
               model.path.compare(model.path.size() - 5, 5, ".onnx") == 0;
    }

    core::Result<std::unique_ptr<Model>> load(const ModelRef& model) override {
        try {
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            // Appended best-first; ONNX Runtime falls through to the CPU for
            // any node a provider cannot take, so this is a preference rather
            // than a requirement.
            for (const std::string& provider : availableProviders()) {
                appendProvider(options, provider);
            }

            auto session = std::make_unique<Ort::Session>(environment(), model.path.c_str(),
                                                          options);

            // The TypeInfo must be NAMED and outlive the shape info.
            //
            // GetTensorTypeAndShapeInfo() returns a view that BORROWS from the
            // TypeInfo it was taken from. Written as one expression, the
            // TypeInfo is a temporary destroyed at the semicolon, and GetShape()
            // then reads freed memory — which surfaced as
            // "cannot create std::vector larger than max_size()" from a
            // dimension count read out of rubble, a whole layer away from the
            // cause.
            Ort::TypeInfo typeInfo = session->GetInputTypeInfo(0);
            const auto info = typeInfo.GetTensorTypeAndShapeInfo();
            const auto shape = info.GetShape();
            if (shape.size() != 4) {
                return core::invalidArgument(model.path +
                                             ": expected a 4-dimensional image input");
            }
            // NCHW when dimension 1 is the channel count; NHWC when dimension 3
            // is. Read from the model rather than assumed, because exports
            // differ and feeding the wrong layout produces confident nonsense
            // rather than an error.
            const bool nchw = shape[1] == 3 || shape[1] == 1;
            const int height = static_cast<int>(nchw ? shape[2] : shape[1]);
            const int width = static_cast<int>(nchw ? shape[3] : shape[2]);
            if (width <= 0 || height <= 0) {
                return core::invalidArgument(
                    model.path + ": dynamic input size is not supported; export with a fixed "
                                 "width and height");
            }

            VS_INFO(kCategory) << "onnxruntime: " << model.path << ' ' << width << 'x' << height
                               << (nchw ? " NCHW" : " NHWC");
            return std::unique_ptr<Model>(new OnnxModel(std::move(session),
                                                        core::Size{width, height},
                                                        core::PixelFormat::RGB888, nchw));
        } catch (const Ort::Exception& error) {
            return core::internalError(std::string("onnxruntime: ") + error.what());
        }
    }

private:
    static void appendProvider(Ort::SessionOptions& options, const std::string& provider) {
        // Each provider has its own append function, and calling one the build
        // does not have throws — which is why availableProviders() asks first.
        try {
            if (provider == "CUDAExecutionProvider") {
                OrtCUDAProviderOptions cuda{};
                options.AppendExecutionProvider_CUDA(cuda);
            } else if (provider == "TensorrtExecutionProvider") {
                OrtTensorRTProviderOptions trt{};
                options.AppendExecutionProvider_TensorRT(trt);
            } else if (provider == "OpenVINOExecutionProvider") {
                options.AppendExecutionProvider("OpenVINO", {});
            } else {
                options.AppendExecutionProvider(provider, {});
            }
        } catch (const Ort::Exception& error) {
            // A provider that is listed but cannot initialise — a CUDA build on
            // a machine with no driver — must not stop the model loading. The
            // CPU below it will take the work.
            VS_WARN(kCategory) << "onnxruntime: " << provider << " unavailable: "
                               << error.what();
        }
    }
};

core::Probe probeOnnxRuntime() {
    try {
        // Touching the environment is what actually proves the library loads.
        (void)environment();
        return core::Probe::yes("ONNX Runtime (" + describeProviders() + ')');
    } catch (const std::exception& error) {
        return core::Probe::no(std::string("ONNX Runtime failed to initialise: ") +
                               error.what());
    }
}

// Priority 20: below fixed-function NPUs, which are dramatically faster for the
// models they support, and above nothing — this is the fallback that makes the
// product run at all on hardware with no dedicated backend.
const core::Register<InferenceBackend> registration({
    "onnxruntime",
    /*priority=*/20,
    &probeOnnxRuntime,
    [] { return std::unique_ptr<InferenceBackend>(new OnnxRuntimeBackend()); },
});

}  // namespace
}  // namespace visora::hal
