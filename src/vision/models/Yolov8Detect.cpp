// YOLOv8 detection, decoded from tensors.
//
// WRITTEN ONCE, RUNS EVERYWHERE. This file names no vendor API: it takes a
// hal::TensorSet and produces detections, so the same decode serves an RKNPU,
// ONNX Runtime, CUDA or a CPU. That is the whole point of abstracting inference
// at the tensor level rather than the model level — the alternative is this
// maths reimplemented per backend, which is how a port becomes untenable.
//
// Quantisation travels with the tensor because this is what needs it: an int8
// output is meaningless without its zero point and scale, and thresholding in
// quantised space (rather than dequantising every value first) is what keeps
// the hot loop cheap.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/ImageMath.hpp"
#include "core/Log.hpp"
#include "vision/ModelType.hpp"
#include "vision/models/YoloCommon.hpp"

namespace visora::vision {
namespace {

constexpr const char* kCategory = "vision";

using yolo::Candidate;
using yolo::kBoxSides;

// One (box, score) pair from the head, at one scale.
struct Head {
    const hal::Tensor* box = nullptr;
    const hal::Tensor* score = nullptr;
};

// Pairs the outputs into heads.
//
// Ordered by grid size rather than by name: exports disagree about names but
// never about shape, and the box tensor of a scale always has 4*bins channels
// while the score tensor has one per class.
std::vector<Head> pairHeads(const hal::TensorSet& tensors) {
    std::vector<Head> heads;
    for (std::size_t i = 0; i + 1 < tensors.outputs.size(); i += 2) {
        const hal::Tensor& first = tensors.outputs[i];
        const hal::Tensor& second = tensors.outputs[i + 1];
        if (first.shape.size() < 4 || second.shape.size() < 4) continue;
        // The box branch has a channel count divisible by four; the score
        // branch is the other one.
        Head head;
        if (first.shape[1] % kBoxSides == 0 && first.shape[1] >= 32) {
            head.box = &first;
            head.score = &second;
        } else {
            head.box = &second;
            head.score = &first;
        }
        heads.push_back(head);
    }
    return heads;
}

class Yolov8Detect final : public ModelType {
public:
    std::string_view id() const override { return "yolov8_detect"; }
    std::string_view label() const override { return "YOLOv8 detection"; }
    std::string_view description() const override {
        return "Object detection. Produces a box, a class and a score for each object.";
    }

    core::Result<std::vector<Detection>> decode(const hal::TensorSet& tensors,
                                                const ModelContext& context) const override {
        const auto heads = pairHeads(tensors);
        if (heads.empty()) {
            return core::invalidArgument(
                "this does not look like a YOLOv8 detection export: expected paired box and "
                "score outputs, got " + std::to_string(tensors.outputs.size()));
        }

        std::vector<Candidate> candidates;
        for (const Head& head : heads) {
            collect(head, context, candidates);
        }

        const auto kept = yolo::suppress(std::move(candidates), context.nmsThreshold);

        // Back to SOURCE coordinates. The model saw a letterboxed image, so
        // undoing the padding and the scale is what makes the boxes line up
        // with the frame an operator is looking at.
        std::vector<Detection> out;
        out.reserve(kept.size());
        for (const Candidate& candidate : kept) {
            Detection detection;
            float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
            core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                              candidate.x1, candidate.y1, &x1, &y1);
            core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                              candidate.x2, candidate.y2, &x2, &y2);
            detection.x1 = x1;
            detection.y1 = y1;
            detection.x2 = x2;
            detection.y2 = y2;
            detection.score = candidate.score;
            detection.classId = candidate.classId;
            out.push_back(std::move(detection));
        }
        return out;
    }

private:
    static void collect(const Head& head, const ModelContext& context,
                        std::vector<Candidate>& out) {
        if (!head.box || !head.score) return;
        if (head.box->type != hal::TensorType::Int8 &&
            head.box->type != hal::TensorType::Float32) {
            VS_WARN(kCategory) << "yolov8: unsupported tensor type "
                               << hal::toString(head.box->type);
            return;
        }

        const int gridH = head.box->shape[2];
        const int gridW = head.box->shape[3];
        const int bins = head.box->shape[1] / kBoxSides;
        const int classes = head.score->shape[1];
        const int cells = gridH * gridW;
        if (cells <= 0 || bins <= 0 || classes <= 0) return;

        // The stride follows from the grid, so this works for any input size
        // without being told what it was.
        const float stride = static_cast<float>(context.inputSize.height) /
                             static_cast<float>(gridH);

        const bool quantised = head.score->type == hal::TensorType::Int8;
        const auto* scoreI8 = static_cast<const std::int8_t*>(head.score->data);
        const auto* scoreF32 = static_cast<const float*>(head.score->data);
        const auto* boxI8 = static_cast<const std::int8_t*>(head.box->data);
        const auto* boxF32 = static_cast<const float*>(head.box->data);
        const std::int8_t thresholdI8 = yolo::quantise(context.confidence, head.score->quant);

        std::vector<float> rawBox(static_cast<std::size_t>(bins) * kBoxSides);
        float sides[kBoxSides];

        for (int cell = 0; cell < cells; ++cell) {
            // Find the best class first, and reject in QUANTISED space. Almost
            // every anchor is background, so this comparison runs 8,400 times
            // per scale and everything after it runs for well under 1%.
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
                bestScore = yolo::dequantise(best, head.score->quant);
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

            for (int k = 0; k < bins * kBoxSides; ++k) {
                rawBox[static_cast<std::size_t>(k)] =
                    quantised ? yolo::dequantise(boxI8[k * cells + cell], head.box->quant)
                              : boxF32[k * cells + cell];
            }
            yolo::decodeDfl(rawBox.data(), bins, sides);

            const float x = static_cast<float>(cell % gridW);
            const float y = static_cast<float>(cell / gridW);
            Candidate candidate;
            candidate.x1 = (-sides[0] + x + 0.5f) * stride;
            candidate.y1 = (-sides[1] + y + 0.5f) * stride;
            candidate.x2 = (sides[2] + x + 0.5f) * stride;
            candidate.y2 = (sides[3] + y + 0.5f) * stride;
            candidate.score = bestScore;
            candidate.classId = bestClass;
            out.push_back(candidate);
        }
    }
};

const core::Register<ModelType> registration({
    "yolov8_detect",
    /*priority=*/0,
    [] { return core::Probe::yes("YOLOv8 detection"); },
    [] { return std::unique_ptr<ModelType>(new Yolov8Detect()); },
});

}  // namespace
}  // namespace visora::vision
