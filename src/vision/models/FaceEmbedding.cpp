// Face embedding (ArcFace, AdaFace and the rest of that family).
//
// NOT A DETECTOR, and the class says so by implementing only enrich(): it
// consumes an aligned face crop and writes a feature vector onto the detection
// it came from. Used as stage zero it produces nothing, which is the honest
// answer rather than a crash.
//
// The vector is written RAW, exactly as the network produced it — no
// normalisation, no truncation to a "standard" length. Comparing two faces is
// a cosine similarity, and whoever does that comparison (the Python consumer,
// today) normalises as part of it. Normalising here as well would be either
// redundant or, if the two disagreed about whether it had already happened,
// silently wrong.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "vision/ModelType.hpp"
#include "vision/models/YoloCommon.hpp"

namespace visora::vision {
namespace {

// Long enough for every embedding network in use (ArcFace is 512); a cap only
// so a misidentified output cannot allocate without bound.
constexpr std::size_t kMaxDimensions = 4096;

class FaceEmbedding final : public ModelType {
public:
    std::string_view id() const override { return "face_recognition"; }
    std::string_view label() const override { return "Face embedding"; }
    std::string_view description() const override {
        return "Turns an aligned face crop into a feature vector for comparison. A later "
               "stage only — as stage zero it finds nothing. Pair it with a face detector "
               "and the 'crop' or an alignment transform.";
    }

    // The face fills the input. These networks are trained on tight, aligned
    // crops, and context around the head is what the alignment removes.
    bool prefersTightCrop() const override { return true; }

    FramePrep framePrep() const override { return FramePrep::Stretch; }

    core::Status enrich(const hal::TensorSet& tensors, const ModelContext& /*context*/,
                        Detection& parent) const override {
        if (tensors.outputs.empty()) {
            return core::invalidArgument("a face embedding network emits one vector; got none");
        }
        const hal::Tensor& vector = tensors.outputs.front();
        if (!yolo::readable(vector)) {
            return core::invalidArgument("the face embedding output cannot be read");
        }

        const std::size_t dimensions = std::min(vector.elementCount(), kMaxDimensions);
        if (dimensions == 0) {
            return core::invalidArgument("the face embedding output is empty");
        }

        parent.embedding.resize(dimensions);
        for (std::size_t i = 0; i < dimensions; ++i) {
            parent.embedding[i] = yolo::valueAt(vector, i);
        }
        return {};
    }
};

const core::Register<ModelType> registration({
    "face_recognition",
    /*priority=*/0,
    [] { return core::Probe::yes("Face embedding"); },
    [] { return std::unique_ptr<ModelType>(new FaceEmbedding()); },
});

}  // namespace
}  // namespace visora::vision
