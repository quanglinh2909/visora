#pragma once

// The arithmetic every YOLOv8-family head shares.
//
// Detection, pose and segmentation are three files because they produce three
// different things, but the box decode underneath them is one thing: a
// Distribution Focal Loss expectation per side, a per-class non-maximum
// suppression, and a dequantisation that lets all of it read int8 tensors off
// an NPU without a conversion pass. Written three times it would drift, and the
// drift would show up as boxes half a pixel apart between two model types on
// the same footage — the kind of difference nobody can explain a year later.
//
// Header-only and free functions: these hold no state, so there is nothing to
// own and nothing to register.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "hal/InferenceBackend.hpp"

namespace visora::vision::yolo {

// A box is four sides, each a DFL distribution of its own.
inline constexpr int kBoxSides = 4;

inline float dequantise(std::int8_t raw, const hal::Quantisation& quant) {
    return (static_cast<float>(raw) - static_cast<float>(quant.zeroPoint)) * quant.scale;
}

// A threshold expressed in the tensor's own quantised space.
//
// Comparing there rather than dequantising every value first is what makes the
// hot loop cheap: a 640x640 frame has 8,400 anchors per scale, and dequantising
// all of them to reject almost all of them is most of the cost.
inline std::int8_t quantise(float value, const hal::Quantisation& quant) {
    if (!quant.quantised()) return 0;
    const float raw = value / quant.scale + static_cast<float>(quant.zeroPoint);
    return static_cast<std::int8_t>(std::clamp(std::lround(raw), -128L, 127L));
}

// Reads element `index` of a tensor whatever its type, dequantising int8.
//
// For the cold paths only — the few anchors that survived a threshold, an OCR
// head of 40x6625 values. A hot loop reads the typed pointer directly and
// compares before dequantising; see the score scan in Yolov8Detect.
inline float valueAt(const hal::Tensor& tensor, std::size_t index) {
    switch (tensor.type) {
        case hal::TensorType::Float32:
            return static_cast<const float*>(tensor.data)[index];
        case hal::TensorType::Int8:
            return dequantise(static_cast<const std::int8_t*>(tensor.data)[index], tensor.quant);
        case hal::TensorType::UInt8:
            return (static_cast<float>(static_cast<const std::uint8_t*>(tensor.data)[index]) -
                    static_cast<float>(tensor.quant.zeroPoint)) *
                   (tensor.quant.quantised() ? tensor.quant.scale : 1.0f);
        case hal::TensorType::Unknown:
            break;
    }
    return 0.0f;
}

inline bool readable(const hal::Tensor& tensor) {
    return tensor.data != nullptr && (tensor.type == hal::TensorType::Float32 ||
                                      tensor.type == hal::TensorType::Int8 ||
                                      tensor.type == hal::TensorType::UInt8);
}

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// Distribution Focal Loss: each side of the box is a probability distribution
// over `bins` integer distances, and the value is its expectation. Softmax then
// weighted sum.
inline void decodeDfl(const float* raw, int bins, float* sides) {
    for (int side = 0; side < kBoxSides; ++side) {
        float sum = 0.0f;
        float accumulated = 0.0f;
        // Softmax needs the exponentials twice, and `bins` is 16 in every
        // YOLOv8 export seen so far — small enough to keep on the stack.
        float exponentials[64];
        if (bins > 64) {
            sides[side] = 0.0f;
            continue;
        }
        for (int i = 0; i < bins; ++i) {
            exponentials[i] = std::exp(raw[i + side * bins]);
            sum += exponentials[i];
        }
        if (sum <= 1e-6f) {
            sides[side] = 0.0f;
            continue;
        }
        for (int i = 0; i < bins; ++i) {
            accumulated += (exponentials[i] / sum) * static_cast<float>(i);
        }
        sides[side] = accumulated;
    }
}

// One surviving anchor, in MODEL INPUT coordinates.
struct Candidate {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float score = 0;
    int classId = 0;
    // Which anchor it came from, counted across every scale in output order.
    //
    // Pose needs it: the keypoint tensor is one flat array over all anchors of
    // all three scales, so the only way back from a surviving box to its
    // seventeen joints is the index it had before suppression reordered
    // everything.
    int anchor = -1;
    // Mask coefficients, for segmentation. Empty for everything else.
    std::vector<float> coefficients;
};

inline float intersectionOverUnion(const Candidate& a, const Candidate& b) {
    const float left = std::max(a.x1, b.x1);
    const float top = std::max(a.y1, b.y1);
    const float right = std::min(a.x2, b.x2);
    const float bottom = std::min(a.y2, b.y2);
    const float overlap = std::max(0.0f, right - left) * std::max(0.0f, bottom - top);
    if (overlap <= 0.0f) return 0.0f;
    const float areaA = (a.x2 - a.x1) * (a.y2 - a.y1);
    const float areaB = (b.x2 - b.x1) * (b.y2 - b.y1);
    const float sum = areaA + areaB - overlap;
    return sum <= 0.0f ? 0.0f : overlap / sum;
}

// Non-maximum suppression, PER CLASS. Across classes would delete a person
// standing in front of a car, which is exactly the case a VMS is for.
inline std::vector<Candidate> suppress(std::vector<Candidate> candidates, float threshold) {
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    std::vector<Candidate> kept;
    std::vector<bool> removed(candidates.size(), false);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (removed[i]) continue;
        kept.push_back(std::move(candidates[i]));
        for (std::size_t j = i + 1; j < candidates.size(); ++j) {
            if (removed[j]) continue;
            if (candidates[j].classId != kept.back().classId) continue;
            if (intersectionOverUnion(kept.back(), candidates[j]) > threshold) {
                removed[j] = true;
            }
        }
    }
    return kept;
}

// Filtering before the expensive part of a decode loop, as well as in the
// runner: rejecting a class here skips the DFL for it.
inline bool passesClass(const std::vector<int>& filter, int classId) {
    if (filter.empty()) return true;
    return std::find(filter.begin(), filter.end(), classId) != filter.end();
}

}  // namespace visora::vision::yolo
