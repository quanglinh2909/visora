// YOLOv8 instance segmentation, decoded from tensors.
//
// The head is the detector's with one extra branch per scale: 32 mask
// COEFFICIENTS per anchor. A separate "proto" output holds 32 basis masks at a
// quarter resolution, and an object's mask is the dot product of its
// coefficients with those bases, thresholded.
//
// WHAT THIS DELIBERATELY DOES NOT DO is paint a full-resolution mask. The wire
// format carries a 32x32 bitmap per object (128 bytes) because that is what an
// overlay on a video tile can render; the predecessor painted a 640x640 label
// image per FRAME and then downsampled it, which is 410 KB of work to produce
// the same 128 bytes. Sampling the proto directly at the 1,024 points the
// bitmap actually has is the same answer for a thousandth of the arithmetic.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/ImageMath.hpp"
#include "vision/ModelType.hpp"
#include "vision/models/YoloCommon.hpp"

namespace visora::vision {
namespace {

// One scale's three or four tensors.
struct Branch {
    const hal::Tensor* box = nullptr;
    const hal::Tensor* score = nullptr;
    const hal::Tensor* coefficients = nullptr;
};

class Yolov8Seg final : public ModelType {
public:
    std::string_view id() const override { return "yolov8_seg"; }
    std::string_view label() const override { return "YOLOv8 segmentation"; }
    std::string_view description() const override {
        return "Object detection with a per-object silhouette. Each detection carries a "
               "32x32 mask bitmap covering its own box.";
    }

    core::Result<std::vector<Detection>> decode(const hal::TensorSet& tensors,
                                                const ModelContext& context) const override {
        // Three scales plus one proto. The per-scale group is three tensors
        // (box, score, coefficients) or four when the export also emits the
        // per-anchor score sum — either way the coefficients are last in the
        // group, which is what makes one index expression cover both.
        const std::size_t count = tensors.outputs.size();
        if (count < 10 || (count - 1) % 3 != 0) {
            return core::invalidArgument(
                "this does not look like a YOLOv8 segmentation export: expected three scales "
                "plus a proto output, got " + std::to_string(count) + " tensors");
        }
        const std::size_t perBranch = (count - 1) / 3;
        const hal::Tensor& proto = tensors.outputs.back();
        if (!yolo::readable(proto) || proto.shape.size() != 4) {
            return core::invalidArgument("the proto output is missing or not [1, C, H, W]");
        }

        std::vector<yolo::Candidate> candidates;
        for (std::size_t scale = 0; scale < 3; ++scale) {
            Branch branch;
            branch.box = &tensors.outputs[scale * perBranch];
            branch.score = &tensors.outputs[scale * perBranch + 1];
            branch.coefficients = &tensors.outputs[scale * perBranch + perBranch - 1];
            collect(branch, context, candidates);
        }

        const auto kept = yolo::suppress(std::move(candidates), context.nmsThreshold);

        std::vector<Detection> out;
        out.reserve(kept.size());
        for (const yolo::Candidate& candidate : kept) {
            Detection detection;
            core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                              candidate.x1, candidate.y1, &detection.x1, &detection.y1);
            core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                              candidate.x2, candidate.y2, &detection.x2, &detection.y2);
            detection.score = candidate.score;
            detection.classId = candidate.classId;
            // Painted from the MODEL-space box, because that is the space the
            // proto lives in. The mapped box above is the same rectangle in
            // frame coordinates; using it here would sample the wrong place on
            // any camera whose frame is not the model input size.
            detection.maskBits = paint(proto, candidate, context);
            out.push_back(std::move(detection));
        }
        return out;
    }

private:
    static void collect(const Branch& branch, const ModelContext& context,
                        std::vector<yolo::Candidate>& out) {
        if (branch.box == nullptr || branch.score == nullptr ||
            branch.coefficients == nullptr) {
            return;
        }
        if (!yolo::readable(*branch.box) || !yolo::readable(*branch.score) ||
            !yolo::readable(*branch.coefficients)) {
            return;
        }
        if (branch.box->shape.size() != 4 || branch.score->shape.size() != 4 ||
            branch.coefficients->shape.size() != 4) {
            return;
        }

        const int gridH = branch.box->shape[2];
        const int gridW = branch.box->shape[3];
        const int cells = gridH * gridW;
        const int bins = branch.box->shape[1] / yolo::kBoxSides;
        const int classes = branch.score->shape[1];
        const int coefficients = branch.coefficients->shape[1];
        if (cells <= 0 || bins <= 0 || classes <= 0 || coefficients <= 0) return;

        const float stride =
            static_cast<float>(context.inputSize.height) / static_cast<float>(gridH);

        const bool quantised = branch.score->type == hal::TensorType::Int8;
        const auto* scoreI8 = static_cast<const std::int8_t*>(branch.score->data);
        const auto* scoreF32 = static_cast<const float*>(branch.score->data);
        const std::int8_t thresholdI8 = yolo::quantise(context.confidence, branch.score->quant);

        std::vector<float> rawBox(static_cast<std::size_t>(bins) * yolo::kBoxSides);
        float sides[yolo::kBoxSides];

        for (int cell = 0; cell < cells; ++cell) {
            int bestClass = -1;
            float bestScore = 0.0f;
            if (quantised) {
                std::int8_t best = thresholdI8;
                for (int c = 0; c < classes; ++c) {
                    const std::int8_t value = scoreI8[c * cells + cell];
                    if (value > best) {
                        best = value;
                        bestClass = c;
                    }
                }
                if (bestClass < 0) continue;
                bestScore = yolo::dequantise(best, branch.score->quant);
            } else {
                for (int c = 0; c < classes; ++c) {
                    const float value = scoreF32[c * cells + cell];
                    if (value > bestScore) {
                        bestScore = value;
                        bestClass = c;
                    }
                }
                if (bestClass < 0 || bestScore < context.confidence) continue;
            }

            if (!yolo::passesClass(context.classFilter, bestClass)) continue;

            for (int k = 0; k < bins * yolo::kBoxSides; ++k) {
                rawBox[static_cast<std::size_t>(k)] =
                    yolo::valueAt(*branch.box, static_cast<std::size_t>(k) * cells + cell);
            }
            yolo::decodeDfl(rawBox.data(), bins, sides);

            const float x = static_cast<float>(cell % gridW);
            const float y = static_cast<float>(cell / gridW);
            yolo::Candidate candidate;
            candidate.x1 = (-sides[0] + x + 0.5f) * stride;
            candidate.y1 = (-sides[1] + y + 0.5f) * stride;
            candidate.x2 = (sides[2] + x + 0.5f) * stride;
            candidate.y2 = (sides[3] + y + 0.5f) * stride;
            candidate.score = bestScore;
            candidate.classId = bestClass;
            candidate.coefficients.resize(static_cast<std::size_t>(coefficients));
            for (int c = 0; c < coefficients; ++c) {
                candidate.coefficients[static_cast<std::size_t>(c)] = yolo::valueAt(
                    *branch.coefficients, static_cast<std::size_t>(c) * cells + cell);
            }
            out.push_back(std::move(candidate));
        }
    }

    // The 32x32 bitmap for one object, sampled at the centre of each grid cell.
    //
    // Bit order is row-major with the low bit of each byte first, which is the
    // predecessor's and therefore what the Python consumer already unpacks.
    static std::vector<std::uint8_t> paint(const hal::Tensor& proto,
                                           const yolo::Candidate& candidate,
                                           const ModelContext& context) {
        constexpr int kGrid = Detection::kMaskGrid;
        const int channels = proto.shape[1];
        const int protoH = proto.shape[2];
        const int protoW = proto.shape[3];
        if (channels <= 0 || protoH <= 0 || protoW <= 0) return {};
        if (static_cast<int>(candidate.coefficients.size()) < channels) return {};

        const float modelW = static_cast<float>(context.inputSize.width);
        const float modelH = static_cast<float>(context.inputSize.height);
        const float x1 = std::clamp(candidate.x1, 0.0f, modelW);
        const float y1 = std::clamp(candidate.y1, 0.0f, modelH);
        const float x2 = std::clamp(candidate.x2, 0.0f, modelW);
        const float y2 = std::clamp(candidate.y2, 0.0f, modelH);
        if (x2 <= x1 || y2 <= y1) return {};

        const std::size_t plane = static_cast<std::size_t>(protoH) * protoW;
        std::vector<std::uint8_t> bits(static_cast<std::size_t>(kGrid) * kGrid / 8, 0);
        bool any = false;

        for (int gy = 0; gy < kGrid; ++gy) {
            const float sampleY = y1 + (y2 - y1) * (static_cast<float>(gy) + 0.5f) / kGrid;
            const int py = std::clamp(static_cast<int>(sampleY / modelH * protoH), 0, protoH - 1);
            for (int gx = 0; gx < kGrid; ++gx) {
                const float sampleX = x1 + (x2 - x1) * (static_cast<float>(gx) + 0.5f) / kGrid;
                const int px =
                    std::clamp(static_cast<int>(sampleX / modelW * protoW), 0, protoW - 1);

                const std::size_t at = static_cast<std::size_t>(py) * protoW + px;
                float sum = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    sum += candidate.coefficients[static_cast<std::size_t>(c)] *
                           yolo::valueAt(proto, static_cast<std::size_t>(c) * plane + at);
                }
                // sigmoid is monotonic and sigmoid(0) = 0.5, so "sigmoid(sum)
                // above a half" is exactly "sum above zero" — one exp() saved
                // per sample, and the same answer.
                if (sum < 0.0f) continue;
                const int bit = gy * kGrid + gx;
                bits[static_cast<std::size_t>(bit >> 3)] |=
                    static_cast<std::uint8_t>(1u << (bit & 7));
                any = true;
            }
        }
        // No pixel of this object inside its own box means the coefficients and
        // the box disagree. Sending 128 zero bytes would say "an empty
        // silhouette" where the truth is "no silhouette".
        return any ? bits : std::vector<std::uint8_t>{};
    }
};

const core::Register<ModelType> registration({
    "yolov8_seg",
    /*priority=*/0,
    [] { return core::Probe::yes("YOLOv8 segmentation"); },
    [] { return std::unique_ptr<ModelType>(new Yolov8Seg()); },
});

}  // namespace
}  // namespace visora::vision
