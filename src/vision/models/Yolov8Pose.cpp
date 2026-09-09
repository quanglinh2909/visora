// YOLOv8 pose, decoded from tensors.
//
// The box decode is the detector's, shared through YoloCommon; what is new here
// is the KEYPOINT tensor, and the awkward thing about it is that it is one flat
// array over every anchor of every scale. A surviving box therefore has to
// remember which anchor it came from — suppression sorts by score and destroys
// the ordering — which is why yolo::Candidate carries an anchor index.
//
// A pose export is single-class by construction: the head emits 64 box channels
// and ONE score channel, "person". That is not an assumption this file makes,
// it is what the tensor shape says, and a multi-class export would fail the
// channel check rather than be silently misread.

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

// Each keypoint is (x, y, score).
constexpr int kKeypointStride = 3;

// The tensor holding the joints: [1, joints, 3, anchors].
//
// Found by shape rather than by index, because an export that emits its scales
// in a different order still puts the joints in the one tensor whose third
// dimension is 3. Falling back to the last output matches every export seen so
// far and keeps a differently-named model working.
const hal::Tensor* findKeypoints(const hal::TensorSet& tensors) {
    for (const hal::Tensor& tensor : tensors.outputs) {
        if (tensor.shape.size() == 4 && tensor.shape[2] == kKeypointStride) return &tensor;
    }
    return tensors.outputs.empty() ? nullptr : &tensors.outputs.back();
}

class Yolov8Pose final : public ModelType {
public:
    std::string_view id() const override { return "yolov8_pose"; }
    std::string_view label() const override { return "YOLOv8 pose"; }
    std::string_view description() const override {
        return "Person detection with skeleton keypoints. One box per person, plus a "
               "(x, y, score) triple for each joint.";
    }

    core::Result<std::vector<Detection>> decode(const hal::TensorSet& tensors,
                                                const ModelContext& context) const override {
        const hal::Tensor* joints = findKeypoints(tensors);
        if (joints == nullptr || !yolo::readable(*joints) || joints->shape.size() != 4) {
            return core::invalidArgument(
                "this does not look like a YOLOv8 pose export: no [1, joints, 3, anchors] "
                "output among the " + std::to_string(tensors.outputs.size()) + " produced");
        }

        const int jointCount = joints->shape[1];
        const int anchorCount = joints->shape[3];
        if (jointCount <= 0 || anchorCount <= 0) {
            return core::invalidArgument("the keypoint output has a degenerate shape");
        }

        std::vector<yolo::Candidate> candidates;
        int anchorBase = 0;
        int branches = 0;
        for (const hal::Tensor& branch : tensors.outputs) {
            if (&branch == joints) continue;
            if (branch.shape.size() != 4) continue;
            const int consumed = collect(branch, context, anchorBase, candidates);
            if (consumed < 0) continue;
            anchorBase += consumed;
            ++branches;
        }
        if (branches == 0) {
            return core::invalidArgument("no usable pose branch among the outputs");
        }
        // The scales must together account for exactly the anchors the keypoint
        // tensor holds. When they do not, an output was skipped or reordered
        // and every joint would be read from the wrong anchor — which looks
        // like a plausible skeleton in the wrong place, the worst kind of
        // wrong.
        if (anchorBase != anchorCount) {
            return core::invalidArgument(
                "pose outputs disagree: the box branches cover " + std::to_string(anchorBase) +
                " anchors, the keypoint output has " + std::to_string(anchorCount));
        }

        const auto kept = yolo::suppress(std::move(candidates), context.nmsThreshold);

        std::vector<Detection> out;
        out.reserve(kept.size());
        for (const yolo::Candidate& candidate : kept) {
            Detection detection;
            mapBox(context, candidate, detection);
            detection.score = candidate.score;
            detection.classId = candidate.classId;
            detection.keypoints = readJoints(*joints, jointCount, anchorCount, candidate.anchor,
                                             context);
            out.push_back(std::move(detection));
        }
        return out;
    }

private:
    // Returns how many anchors this branch covered, or -1 when it is not one.
    static int collect(const hal::Tensor& branch, const ModelContext& context, int anchorBase,
                       std::vector<yolo::Candidate>& out) {
        if (!yolo::readable(branch)) return -1;
        const int channels = branch.shape[1];
        const int gridH = branch.shape[2];
        const int gridW = branch.shape[3];
        const int cells = gridH * gridW;
        if (cells <= 0 || channels < yolo::kBoxSides + 1) return -1;

        // 64 box channels then one score channel. Anything else is not a pose
        // branch, and reading it as one would produce joints for boxes that
        // came from somewhere else.
        const int locLen = channels - 1;
        if (locLen % yolo::kBoxSides != 0) return -1;
        const int bins = locLen / yolo::kBoxSides;

        const float stride =
            static_cast<float>(context.inputSize.height) / static_cast<float>(gridH);

        const bool quantised = branch.type == hal::TensorType::Int8;
        const auto* i8 = static_cast<const std::int8_t*>(branch.data);
        const auto* f32 = static_cast<const float*>(branch.data);

        // The score channel is a LOGIT here, not a probability — pose applies
        // the sigmoid in postprocessing where detection does not. So the gate
        // is the threshold's logit, quantised, minus one step for rounding.
        // Anything below it cannot reach the threshold; anything above is
        // checked again in float, so the gate can only save work, never change
        // the answer.
        std::int8_t gate = -128;
        if (quantised) {
            if (!(context.confidence > 0.0f && context.confidence < 1.0f)) return -1;
            const float logit = std::log(context.confidence / (1.0f - context.confidence));
            const float q = logit / branch.quant.scale + static_cast<float>(branch.quant.zeroPoint);
            if (q > 127.0f) return cells;  // no anchor here can reach the threshold
            gate = q > -128.0f ? static_cast<std::int8_t>(std::floor(q) - 1.0f) : -128;
        }

        std::vector<float> rawBox(static_cast<std::size_t>(locLen));
        float sides[yolo::kBoxSides];

        for (int cell = 0; cell < cells; ++cell) {
            float logit = 0.0f;
            if (quantised) {
                const std::int8_t raw = i8[locLen * cells + cell];
                if (raw < gate) continue;
                logit = yolo::dequantise(raw, branch.quant);
            } else {
                logit = f32[locLen * cells + cell];
            }
            const float score = yolo::sigmoid(logit);
            if (score < context.confidence) continue;
            if (!yolo::passesClass(context.classFilter, 0)) continue;

            for (int k = 0; k < locLen; ++k) {
                rawBox[static_cast<std::size_t>(k)] =
                    quantised ? yolo::dequantise(i8[k * cells + cell], branch.quant)
                              : f32[k * cells + cell];
            }
            yolo::decodeDfl(rawBox.data(), bins, sides);

            const float x = static_cast<float>(cell % gridW);
            const float y = static_cast<float>(cell / gridW);
            yolo::Candidate candidate;
            candidate.x1 = (-sides[0] + x + 0.5f) * stride;
            candidate.y1 = (-sides[1] + y + 0.5f) * stride;
            candidate.x2 = (sides[2] + x + 0.5f) * stride;
            candidate.y2 = (sides[3] + y + 0.5f) * stride;
            candidate.score = score;
            candidate.classId = 0;
            candidate.anchor = anchorBase + cell;
            out.push_back(std::move(candidate));
        }
        return cells;
    }

    static void mapBox(const ModelContext& context, const yolo::Candidate& candidate,
                       Detection& detection) {
        core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                          candidate.x1, candidate.y1, &detection.x1, &detection.y1);
        core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                          candidate.x2, candidate.y2, &detection.x2, &detection.y2);
    }

    // Joints come out in MODEL INPUT pixels, so they map back exactly the way a
    // box does. Doing it any other way — scaling by the box, say — is how a
    // skeleton ends up correct on a square camera and sheared on a 16:9 one.
    static std::vector<float> readJoints(const hal::Tensor& joints, int jointCount,
                                         int anchorCount, int anchor,
                                         const ModelContext& context) {
        std::vector<float> out;
        if (anchor < 0 || anchor >= anchorCount) return out;
        out.reserve(static_cast<std::size_t>(jointCount) * kKeypointStride);

        const std::size_t plane = static_cast<std::size_t>(anchorCount);
        for (int joint = 0; joint < jointCount; ++joint) {
            const std::size_t base =
                static_cast<std::size_t>(joint) * kKeypointStride * plane +
                static_cast<std::size_t>(anchor);
            const float modelX = yolo::valueAt(joints, base);
            const float modelY = yolo::valueAt(joints, base + plane);
            const float score = yolo::valueAt(joints, base + 2 * plane);

            float x = 0.0f;
            float y = 0.0f;
            core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                              modelX, modelY, &x, &y);
            out.push_back(x);
            out.push_back(y);
            out.push_back(score);
        }
        return out;
    }
};

const core::Register<ModelType> registration({
    "yolov8_pose",
    /*priority=*/0,
    [] { return core::Probe::yes("YOLOv8 pose"); },
    [] { return std::unique_ptr<ModelType>(new Yolov8Pose()); },
});

}  // namespace
}  // namespace visora::vision
