// PP-OCR text detection (DBNet), decoded from tensors.
//
// The model emits ONE probability map at a quarter of the input resolution:
// "how likely is this pixel part of a character". Turning that into text lines
// is four steps, and each one exists because of something measured on real
// plates:
//
//   1. threshold at 0.3 — PaddleOCR's own default;
//   2. group the surviving pixels into connected blobs (8-way, iterative: a
//      long line of text is tens of thousands of pixels and recursion per pixel
//      overflows the stack);
//   3. MERGE blobs into lines. DBNet is trained to output SHRUNK character
//      regions, so on a small or thin-stroked image every character comes out
//      as its own blob — a 128x110 plate measured 20 fragments, and feeding
//      each fragment to the recogniser produced nothing at all;
//   4. unclip, because what survives is that shrunken region and the
//      recogniser needs the actual glyphs.
//
// This is the "det" half of PP-OCR. The recogniser is a separate model, and in
// this system a separate STAGE: det finds the lines, rec reads each one. The
// predecessor also had a combined "paddle_ocr" type that ran both models behind
// one name; a stage tree expresses the same thing without a special case, so it
// is not ported.

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

// Above this, a pixel is text. PaddleOCR's default.
constexpr float kBinaryThreshold = 0.3f;
// A blob smaller than this is noise; a few pixels are never a readable glyph.
constexpr int kMinimumArea = 12;
// DBNet learns the SHRUNK region, so the box has to be grown back. 1.6 is the
// ratio PaddleOCR uses.
constexpr float kUnclipRatio = 1.6f;
// A guard, not a tuning knob: a map full of noise must not turn into thousands
// of boxes each of which costs a recogniser inference downstream.
constexpr std::size_t kMaxBoxes = 64;

struct Blob {
    int left = 0, top = 0, right = 0, bottom = 0;
    int area = 0;
    double probabilitySum = 0.0;
};

void collectBlobs(const std::vector<float>& probability, int width, int height,
                  std::vector<Blob>& out) {
    std::vector<std::uint8_t> seen(probability.size(), 0);
    std::vector<int> stack;
    stack.reserve(1024);

    for (int y0 = 0; y0 < height; ++y0) {
        for (int x0 = 0; x0 < width; ++x0) {
            const std::size_t start = static_cast<std::size_t>(y0) * width + x0;
            if (seen[start] || probability[start] < kBinaryThreshold) continue;

            Blob blob{x0, y0, x0, y0, 0, 0.0};
            seen[start] = 1;
            stack.clear();
            stack.push_back(static_cast<int>(start));
            while (!stack.empty()) {
                const int index = stack.back();
                stack.pop_back();
                const int x = index % width;
                const int y = index / width;
                ++blob.area;
                blob.probabilitySum += probability[static_cast<std::size_t>(index)];
                blob.left = std::min(blob.left, x);
                blob.right = std::max(blob.right, x);
                blob.top = std::min(blob.top, y);
                blob.bottom = std::max(blob.bottom, y);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                        const std::size_t at = static_cast<std::size_t>(ny) * width + nx;
                        if (seen[at] || probability[at] < kBinaryThreshold) continue;
                        seen[at] = 1;
                        stack.push_back(static_cast<int>(at));
                    }
                }
            }
            if (blob.area >= kMinimumArea) out.push_back(blob);
        }
    }
}

// Merges fragments that are plainly the same LINE of text.
//
// Two conditions, both measured rather than guessed: they must overlap
// vertically by more than half the shorter one (same line), and the horizontal
// gap must be under 0.6 of the taller one (normal letter or word spacing).
// Height is the yardstick, so this scales with the text rather than the image.
//
// The third condition, that the two be of SIMILAR HEIGHT, is what stops a tall
// blob — a plate border, a table rule — swallowing everything near it. Without
// it a 128x110 plate produced a single box covering the whole picture.
void mergeIntoLines(std::vector<Blob>& blobs) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < blobs.size(); ++i) {
            for (std::size_t j = i + 1; j < blobs.size();) {
                Blob& a = blobs[i];
                const Blob& b = blobs[j];
                const int heightA = a.bottom - a.top + 1;
                const int heightB = b.bottom - b.top + 1;
                const int overlap = std::min(a.bottom, b.bottom) - std::max(a.top, b.top) + 1;
                const int shorter = std::min(heightA, heightB);
                const int taller = std::max(heightA, heightB);
                const int gap = std::max(a.left, b.left) - std::min(a.right, b.right) - 1;

                const bool similarHeight = shorter * 2 >= taller;
                if (similarHeight && overlap > shorter / 2 &&
                    gap <= static_cast<int>(0.6f * static_cast<float>(taller))) {
                    a.left = std::min(a.left, b.left);
                    a.top = std::min(a.top, b.top);
                    a.right = std::max(a.right, b.right);
                    a.bottom = std::max(a.bottom, b.bottom);
                    a.area += b.area;
                    a.probabilitySum += b.probabilitySum;
                    blobs.erase(blobs.begin() + static_cast<long>(j));
                    changed = true;
                    continue;
                }
                ++j;
            }
        }
    }
}

class PpOcrDetect final : public ModelType {
public:
    std::string_view id() const override { return "ppocr_det"; }
    std::string_view label() const override { return "PP-OCR text detection"; }
    std::string_view description() const override {
        return "Finds lines of text and returns a box for each. Pair it with a "
               "'ppocr_rec' stage to read them.";
    }

    core::Result<std::vector<Detection>> decode(const hal::TensorSet& tensors,
                                                const ModelContext& context) const override {
        if (tensors.outputs.empty()) {
            return core::invalidArgument("a PP-OCR detector emits one probability map; got none");
        }
        const hal::Tensor& map = tensors.outputs.front();
        if (!yolo::readable(map) || map.shape.size() < 2) {
            return core::invalidArgument("the PP-OCR probability map is missing or malformed");
        }
        // The last two dimensions, whatever the batch and channel dims look
        // like: exports disagree about [1,1,H,W] versus [1,H,W].
        const int height = map.shape[map.shape.size() - 2];
        const int width = map.shape[map.shape.size() - 1];
        if (width <= 0 || height <= 0) {
            return core::invalidArgument("the PP-OCR probability map has a degenerate shape");
        }

        // Dequantised once into a plain array. The blob walk touches each pixel
        // several times over, and paying the int8 conversion on every visit
        // would cost more than the whole search.
        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        std::vector<float> probability(pixels);
        for (std::size_t i = 0; i < pixels; ++i) probability[i] = yolo::valueAt(map, i);

        std::vector<Blob> blobs;
        collectBlobs(probability, width, height, blobs);
        mergeIntoLines(blobs);

        // Strongest first, so the cap keeps the lines most likely to be text
        // rather than whichever the raster order happened to reach.
        std::sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) {
            return a.probabilitySum / std::max(1, a.area) >
                   b.probabilitySum / std::max(1, b.area);
        });

        const float scaleX = static_cast<float>(context.inputSize.width) / static_cast<float>(width);
        const float scaleY =
            static_cast<float>(context.inputSize.height) / static_cast<float>(height);

        std::vector<Detection> out;
        for (const Blob& blob : blobs) {
            if (out.size() >= kMaxBoxes) break;
            const float score =
                static_cast<float>(blob.probabilitySum / static_cast<double>(blob.area));
            if (score < context.confidence) continue;

            const int blobWidth = blob.right - blob.left + 1;
            const int blobHeight = blob.bottom - blob.top + 1;
            if (blobWidth < 3 || blobHeight < 3) continue;

            // The unclip distance DBNet specifies: area * ratio / perimeter,
            // computed on the bounding rectangle.
            const double perimeter = 2.0 * (blobWidth + blobHeight);
            const int grow =
                perimeter > 0.0
                    ? static_cast<int>(static_cast<double>(blobWidth) * blobHeight *
                                       kUnclipRatio / perimeter)
                    : 0;

            const float modelX1 = static_cast<float>(blob.left - grow) * scaleX;
            const float modelY1 = static_cast<float>(blob.top - grow) * scaleY;
            const float modelX2 = static_cast<float>(blob.right + grow + 1) * scaleX;
            const float modelY2 = static_cast<float>(blob.bottom + grow + 1) * scaleY;

            Detection detection;
            core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                              modelX1, modelY1, &detection.x1, &detection.y1);
            core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                              modelX2, modelY2, &detection.x2, &detection.y2);
            if (detection.x2 <= detection.x1 || detection.y2 <= detection.y1) continue;
            detection.score = score;
            // One class: "there is text here". Which text is the recogniser's
            // answer, not this one's.
            detection.classId = 0;
            if (!yolo::passesClass(context.classFilter, detection.classId)) continue;
            out.push_back(std::move(detection));
        }
        return out;
    }
};

const core::Register<ModelType> registration({
    "ppocr_det",
    /*priority=*/0,
    [] { return core::Probe::yes("PP-OCR text detection"); },
    [] { return std::unique_ptr<ModelType>(new PpOcrDetect()); },
});

}  // namespace
}  // namespace visora::vision
