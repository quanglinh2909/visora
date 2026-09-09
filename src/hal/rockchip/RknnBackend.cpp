// Rockchip NPU (RKNN) as an InferenceBackend.
//
// Loads a .rknn artefact and hands back raw tensors with their quantisation
// parameters attached. It deliberately does no postprocessing: decoding boxes,
// NMS, pose and OCR heads are model concerns that live one layer up and are
// written once for every backend. That split is the whole reason this interface
// is at the tensor level.

#include <rknn_api.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Log.hpp"
#include "hal/InferenceBackend.hpp"

namespace visora::hal::rockchip {
namespace {

constexpr const char* kCategory = "rknn";

bool endsWith(const std::string& text, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

TensorType toTensorType(rknn_tensor_type type) {
    switch (type) {
        case RKNN_TENSOR_INT8:    return TensorType::Int8;
        case RKNN_TENSOR_UINT8:   return TensorType::UInt8;
        case RKNN_TENSOR_FLOAT32: return TensorType::Float32;
        default:                  return TensorType::Unknown;
    }
}

std::size_t bytesPerElement(TensorType type) {
    switch (type) {
        case TensorType::Int8:
        case TensorType::UInt8:   return 1;
        case TensorType::Float32: return 4;
        case TensorType::Unknown: return 0;
    }
    return 0;
}

std::vector<std::uint8_t> readFile(const std::string& path, std::string* error) {
    std::vector<std::uint8_t> bytes;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        *error = path + ": " + std::strerror(errno);
        return bytes;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        *error = path + ": empty file";
        std::fclose(file);
        return bytes;
    }
    bytes.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        *error = path + ": short read";
        bytes.clear();
    }
    return bytes;
}

class RknnModel final : public Model {
public:
    RknnModel(rknn_context ctx, std::vector<rknn_tensor_attr> inputs,
              std::vector<rknn_tensor_attr> outputs, bool quantised, core::Size inputSize,
              core::PixelFormat inputFormat, std::string name)
        : m_ctx(ctx),
          m_inputs(std::move(inputs)),
          m_outputs(std::move(outputs)),
          m_quantised(quantised),
          m_inputSize(inputSize),
          m_inputFormat(inputFormat),
          m_name(std::move(name)) {}

    ~RknnModel() override {
        if (m_ctx != 0) rknn_destroy(m_ctx);
    }

    core::Size inputSize() const override { return m_inputSize; }
    core::PixelFormat inputFormat() const override { return m_inputFormat; }

    core::Result<TensorSet> run(const core::ImageView& input) override {
        if (!input.hasCpu()) {
            return core::unsupported("rknn needs a mapped input image");
        }
        if (input.size.width != m_inputSize.width || input.size.height != m_inputSize.height) {
            return core::invalidArgument(
                m_name + ": input is " + std::to_string(input.size.width) + "x" +
                std::to_string(input.size.height) + ", model wants " +
                std::to_string(m_inputSize.width) + "x" + std::to_string(m_inputSize.height) +
                " (fitting the image is the ImageOps layer's job)");
        }

        // One inference at a time per model context. The NPU driver serialises
        // anyway, and sharing a context across threads without this corrupts it.
        std::lock_guard<std::mutex> lock(m_mutex);

        rknn_input in{};
        in.index = 0;
        in.type = RKNN_TENSOR_UINT8;
        in.fmt = RKNN_TENSOR_NHWC;
        in.size = static_cast<std::uint32_t>(core::packedSize(input.format, input.size));
        in.buf = const_cast<std::uint8_t*>(input.data);

        int ret = rknn_inputs_set(m_ctx, 1, &in);
        if (ret < 0) {
            return core::hardwareFailure(m_name + ": rknn_inputs_set failed (" +
                                         std::to_string(ret) + ")");
        }

        ret = rknn_run(m_ctx, nullptr);
        if (ret < 0) {
            return core::hardwareFailure(m_name + ": rknn_run failed (" + std::to_string(ret) +
                                         ")");
        }

        const std::size_t count = m_outputs.size();
        std::vector<rknn_output> raw(count);
        for (std::size_t i = 0; i < count; ++i) {
            raw[i].index = static_cast<std::uint32_t>(i);
            // Quantised models keep their int8 output: dequantising every
            // element here would burn CPU on values the postprocessor discards
            // after thresholding. The scale and zero point travel with the
            // tensor so it can dequantise only what it keeps.
            raw[i].want_float = m_quantised ? 0 : 1;
            raw[i].is_prealloc = 0;
        }

        ret = rknn_outputs_get(m_ctx, static_cast<std::uint32_t>(count), raw.data(), nullptr);
        if (ret < 0) {
            return core::hardwareFailure(m_name + ": rknn_outputs_get failed (" +
                                         std::to_string(ret) + ")");
        }

        TensorSet set;
        set.outputs.reserve(count);
        set.storage.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            const rknn_tensor_attr& attr = m_outputs[i];

            Tensor tensor;
            tensor.name = attr.name;
            tensor.type = m_quantised ? toTensorType(attr.type) : TensorType::Float32;
            tensor.shape.assign(attr.dims, attr.dims + attr.n_dims);
            if (m_quantised && attr.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
                tensor.quant.scale = attr.scale;
                tensor.quant.zeroPoint = attr.zp;
            }

            // Copy out before releasing: the driver's buffers are reused on the
            // next inference, and TensorSet promises the caller may keep it.
            const std::size_t bytes = raw[i].size;
            set.storage.emplace_back(static_cast<const std::uint8_t*>(raw[i].buf),
                                     static_cast<const std::uint8_t*>(raw[i].buf) + bytes);
            tensor.data = set.storage.back().data();
            tensor.byteCount = bytes;
            set.outputs.push_back(std::move(tensor));
        }

        rknn_outputs_release(m_ctx, static_cast<std::uint32_t>(count), raw.data());
        return set;
    }

private:
    rknn_context m_ctx = 0;
    std::vector<rknn_tensor_attr> m_inputs;
    std::vector<rknn_tensor_attr> m_outputs;
    bool m_quantised = false;
    core::Size m_inputSize;
    core::PixelFormat m_inputFormat = core::PixelFormat::RGB888;
    std::string m_name;
    std::mutex m_mutex;
};

class RknnBackend final : public InferenceBackend {
public:
    std::string_view id() const override { return "rknn"; }

    bool handles(const ModelRef& model) const override { return endsWith(model.path, ".rknn"); }

    core::Result<std::unique_ptr<Model>> load(const ModelRef& model) override {
        std::string error;
        std::vector<std::uint8_t> bytes = readFile(model.path, &error);
        if (bytes.empty()) return core::notFound(error);

        rknn_context ctx = 0;
        int ret = rknn_init(&ctx, bytes.data(), static_cast<std::uint32_t>(bytes.size()), 0,
                            nullptr);
        if (ret < 0) {
            return core::hardwareFailure(model.path + ": rknn_init failed (" +
                                         std::to_string(ret) + ")");
        }

        rknn_input_output_num ioNum{};
        ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ioNum, sizeof(ioNum));
        if (ret < 0) {
            rknn_destroy(ctx);
            return core::hardwareFailure(model.path + ": RKNN_QUERY_IN_OUT_NUM failed");
        }
        if (ioNum.n_input < 1) {
            rknn_destroy(ctx);
            return core::invalidArgument(model.path + ": model has no input");
        }

        std::vector<rknn_tensor_attr> inputs(ioNum.n_input);
        for (std::uint32_t i = 0; i < ioNum.n_input; ++i) {
            inputs[i].index = i;
            if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &inputs[i], sizeof(rknn_tensor_attr)) < 0) {
                rknn_destroy(ctx);
                return core::hardwareFailure(model.path + ": RKNN_QUERY_INPUT_ATTR failed");
            }
        }
        std::vector<rknn_tensor_attr> outputs(ioNum.n_output);
        for (std::uint32_t i = 0; i < ioNum.n_output; ++i) {
            outputs[i].index = i;
            if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &outputs[i], sizeof(rknn_tensor_attr)) <
                0) {
                rknn_destroy(ctx);
                return core::hardwareFailure(model.path + ": RKNN_QUERY_OUTPUT_ATTR failed");
            }
        }

        // Input geometry comes from the model itself rather than from
        // configuration: the two disagreeing is a class of bug worth making
        // impossible.
        const rknn_tensor_attr& in = inputs[0];
        core::Size size;
        if (in.fmt == RKNN_TENSOR_NCHW) {
            size = {static_cast<int>(in.dims[3]), static_cast<int>(in.dims[2])};
        } else {
            size = {static_cast<int>(in.dims[2]), static_cast<int>(in.dims[1])};
        }

        const bool quantised = outputs.empty()
                                   ? false
                                   : (outputs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                                      outputs[0].type == RKNN_TENSOR_INT8);

        VS_INFO(kCategory) << "loaded " << model.path << ": input " << size.width << 'x'
                           << size.height << ", " << ioNum.n_output << " output(s), "
                           << (quantised ? "int8 quantised" : "float32");

        return std::unique_ptr<Model>(new RknnModel(ctx, std::move(inputs), std::move(outputs),
                                                    quantised, size, core::PixelFormat::RGB888,
                                                    model.path));
    }
};

Probe probeRknn() {
    // The library links, but that says nothing about the device: a board with
    // no NPU node, or a container without it mapped in, must not select this.
    const char* version = nullptr;
    rknn_sdk_version sdk{};
    // rknn_query needs a context, and there is none before a model loads, so
    // the honest check is whether the NPU device node exists.
    std::FILE* node = std::fopen("/proc/rknpu/version", "re");
    if (node == nullptr) node = std::fopen("/sys/kernel/debug/rknpu/version", "re");
    if (node == nullptr) {
        // Older BSPs expose neither; fall back to the driver device itself.
        node = std::fopen("/dev/rknpu", "re");
        if (node == nullptr) {
            return Probe::no("no RKNPU device node (/proc/rknpu/version, /dev/rknpu)");
        }
    }
    char line[128] = {0};
    version = std::fgets(line, sizeof(line), node);
    std::fclose(node);

    std::string detail = "RKNPU present";
    if (version != nullptr) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (!text.empty()) detail += ", driver " + text;
    }
    (void)sdk;
    return Probe::yes(detail);
}

const Register<InferenceBackend> registration{{
    "rknn",
    100,
    &probeRknn,
    [] { return std::unique_ptr<InferenceBackend>(new RknnBackend()); },
}};

}  // namespace
}  // namespace visora::hal::rockchip
