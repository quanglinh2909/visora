// PP-OCR text recognition (CTC head), decoded from tensors.
//
// Reads ONE line of text. The head emits `steps` timesteps across the width of
// its input, each a distribution over the character dictionary plus a blank;
// greedy CTC takes the argmax at each step and drops blanks and repeats.
//
// WHY ONE DETECTION PER CHARACTER rather than one box with a string: every
// consumer in this system — the socket JSON, the plate assembler in Python, the
// overlay on the test page — is built around a list of boxes with a class id
// and a score. Emitting per-character boxes means this drops into all of them
// unchanged, and the readable character rides along in Detection::text.
//
// The x of each character is its timestep: step t covers
// [t*W/steps, (t+1)*W/steps] of the model input. That is a real horizontal
// position, accurate to one step (8 px at 48x320), which is what lets the
// assembler sort characters and split a two-line plate.
//
// THE DICTIONARY LIVES BESIDE THE WEIGHTS, and differs per model file, so it is
// loaded from ModelContext::modelPath rather than compiled in. A recogniser
// trained on Vietnamese plates and one trained on Chinese receipts are the same
// code and different sidecars.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/ImageMath.hpp"
#include "core/Log.hpp"
#include "vision/ModelType.hpp"
#include "vision/models/YoloCommon.hpp"

namespace visora::vision {
namespace {

constexpr const char* kCategory = "vision";

// A line has an end. The cap is a guard against a malformed head, not a limit
// on real text.
constexpr int kMaxCharacters = 64;

std::vector<std::string> readDictionary(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in.is_open()) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

class PpOcrRecognise final : public ModelType {
public:
    std::string_view id() const override { return "paddle_ocr_rec"; }
    std::string_view label() const override { return "PP-OCR text recognition"; }
    std::string_view description() const override {
        return "Reads one line of text, returning a box per character with the character "
               "itself. Give it a text crop — from a 'paddle_ocr_det' stage, or a plate from a "
               "detector.";
    }

    // Anchored left at the true aspect ratio, padded on the right — PaddleOCR's
    // own resize_norm_img.
    //
    // Measured on six real number plates: stretching to fill read 0 of 6
    // correctly, this read 5 of 6 at 0.97 confidence, and a CENTRED letterbox
    // read 3 of 6 because the line shrinks and drifts away from the left edge
    // the model was trained to start at.
    FramePrep framePrep() const override { return FramePrep::FitHeight; }

    // The crop must be the text and nothing else. Context around a plate is
    // what a detector wants; here it is just fewer pixels for the glyphs.
    bool prefersTightCrop() const override { return true; }

    core::Result<std::vector<Detection>> decode(const hal::TensorSet& tensors,
                                                const ModelContext& context) const override {
        if (tensors.outputs.empty()) {
            return core::invalidArgument("a PP-OCR recogniser emits one [1, steps, classes] "
                                         "output; got none");
        }
        const hal::Tensor& head = tensors.outputs.front();
        if (!yolo::readable(head) || head.shape.size() < 3) {
            return core::invalidArgument("the PP-OCR recognition output is missing or not "
                                         "[1, steps, classes]");
        }
        const int steps = head.shape[head.shape.size() - 2];
        const int classes = head.shape[head.shape.size() - 1];
        if (steps <= 0 || classes <= 1) {
            return core::invalidArgument("the PP-OCR recognition output has a degenerate shape");
        }

        const std::vector<std::string>& dictionary = dictionaryFor(context.modelPath);

        // The width one timestep covers, in model-input pixels.
        const float stepWidth =
            static_cast<float>(context.inputSize.width) / static_cast<float>(steps);

        std::vector<Detection> out;
        int previous = -1;
        for (int step = 0; step < steps && static_cast<int>(out.size()) < kMaxCharacters; ++step) {
            const std::size_t base = static_cast<std::size_t>(step) * classes;
            int best = 0;
            float bestValue = yolo::valueAt(head, base);
            for (int c = 1; c < classes; ++c) {
                const float value = yolo::valueAt(head, base + static_cast<std::size_t>(c));
                if (value > bestValue) {
                    bestValue = value;
                    best = c;
                }
            }
            // Class 0 is the CTC blank, and a repeat of the previous step is
            // the same character still being emitted — both are dropped. That
            // collapsing is what turns 40 timesteps into 8 characters.
            const int wasPrevious = previous;
            previous = best;
            if (best == 0 || best == wasPrevious) continue;

            const float score = std::min(bestValue, 1.0f);
            if (score < context.confidence) continue;

            const int index = best - 1;  // the blank is not in the dictionary
            if (!yolo::passesClass(context.classFilter, index)) continue;

            const float modelX1 = static_cast<float>(step) * stepWidth;
            const float modelX2 = std::min(modelX1 + stepWidth,
                                           static_cast<float>(context.inputSize.width));

            Detection detection;
            core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                              modelX1, 0.0f, &detection.x1, &detection.y1);
            core::mapToSource(context.contentRect, context.sourceSize, context.inputSize,
                              modelX2, static_cast<float>(context.inputSize.height),
                              &detection.x2, &detection.y2);
            detection.score = score;
            detection.classId = index;
            if (index >= 0 && index < static_cast<int>(dictionary.size())) {
                detection.text = dictionary[static_cast<std::size_t>(index)];
            }
            out.push_back(std::move(detection));
        }
        return out;
    }

private:
    // "<model>.rknn" -> "<model>.txt", one character per line in training
    // order; failing that, "ppocr_keys_v1.txt" beside it, which is the name
    // Rockchip's model zoo ships for PP-OCRv4.
    //
    // Cached per model path. The instance is shared between every job worker,
    // so the cache is guarded — and a job that reloads its model does not
    // re-read a file that has not changed.
    const std::vector<std::string>& dictionaryFor(const std::string& modelPath) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto known = m_dictionaries.find(modelPath);
        if (known != m_dictionaries.end()) return known->second;

        std::string sidecar = modelPath;
        const std::size_t dot = sidecar.find_last_of('.');
        const std::size_t slash = sidecar.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            sidecar.resize(dot);
        }
        sidecar += ".txt";

        std::vector<std::string> dictionary = readDictionary(sidecar);
        if (dictionary.empty()) {
            const std::string directory =
                slash == std::string::npos ? std::string(".") : modelPath.substr(0, slash);
            dictionary = readDictionary(directory + "/ppocr_keys_v1.txt");
        }

        if (dictionary.empty()) {
            VS_WARN(kCategory) << "paddle_ocr_rec: no dictionary beside " << modelPath
                               << " — results carry class ids but no text";
        } else {
            // The trailing space class exists only when the model was trained
            // with use_space_char. Appending it either way is harmless: a class
            // id past the end simply yields no text.
            dictionary.push_back(" ");
            VS_INFO(kCategory) << "paddle_ocr_rec: " << dictionary.size() << " characters from "
                               << sidecar;
        }
        return m_dictionaries.emplace(modelPath, std::move(dictionary)).first->second;
    }

    mutable std::mutex m_mutex;
    mutable std::map<std::string, std::vector<std::string>> m_dictionaries;
};

const core::Register<ModelType> registration({
    "paddle_ocr_rec",
    /*priority=*/0,
    [] { return core::Probe::yes("PP-OCR text recognition"); },
    [] { return std::unique_ptr<ModelType>(new PpOcrRecognise()); },
});

}  // namespace
}  // namespace visora::vision
