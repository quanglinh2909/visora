// The vision layer: model-type and transform registries, stage-tree validation,
// and the YOLOv8 decode.
//
// The decode is asserted against tensors built here rather than against a model
// file, so it runs on a machine with no NPU, no ONNX Runtime and no weights —
// which is the point of abstracting inference at the tensor level.

#include "TestHarness.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "vision/ModelType.hpp"
#include "vision/StageRunner.hpp"
#include "vision/Transform.hpp"

using namespace visora;
using visora::vision::AiStage;

namespace {

// Builds one YOLOv8 head: a box tensor of 4*bins channels and a score tensor of
// one channel per class, both NCHW over a gridW x gridH grid.
//
// A float32 export, because that is the shape an ONNX model has and the int8
// path differs only by the dequantisation the decoder applies.
struct HeadBuilder {
    int gridW = 0;
    int gridH = 0;
    int bins = 16;
    int classes = 4;
    std::vector<float> box;
    std::vector<float> score;

    HeadBuilder(int w, int h, int classCount) : gridW(w), gridH(h), classes(classCount) {
        box.assign(static_cast<std::size_t>(4 * bins * w * h), 0.0f);
        // A large negative logit everywhere: after softmax the DFL expectation
        // is near zero, and the score branch is read directly so this is simply
        // "no object".
        score.assign(static_cast<std::size_t>(classCount * w * h), 0.0f);
    }

    // Puts an object in one cell: a class score and four side distances, each
    // expressed as a one-hot distribution over the DFL bins.
    void put(int cellX, int cellY, int classId, float confidence, int sideBin) {
        const int cells = gridW * gridH;
        const int cell = cellY * gridW + cellX;
        score[static_cast<std::size_t>(classId * cells + cell)] = confidence;
        for (int side = 0; side < 4; ++side) {
            // One-hot in log space: exp() of 20 dominates the softmax, so the
            // expectation lands on `sideBin`.
            box[static_cast<std::size_t>((side * bins + sideBin) * cells + cell)] = 20.0f;
        }
    }

    void appendTo(hal::TensorSet& set) {
        const int cells = gridW * gridH;

        hal::Tensor boxTensor;
        boxTensor.name = "box";
        boxTensor.type = hal::TensorType::Float32;
        boxTensor.shape = {1, 4 * bins, gridH, gridW};
        set.storage.emplace_back(reinterpret_cast<const std::uint8_t*>(box.data()),
                                 reinterpret_cast<const std::uint8_t*>(box.data()) +
                                     box.size() * sizeof(float));
        boxTensor.data = set.storage.back().data();
        boxTensor.byteCount = set.storage.back().size();

        hal::Tensor scoreTensor;
        scoreTensor.name = "score";
        scoreTensor.type = hal::TensorType::Float32;
        scoreTensor.shape = {1, classes, gridH, gridW};
        set.storage.emplace_back(reinterpret_cast<const std::uint8_t*>(score.data()),
                                 reinterpret_cast<const std::uint8_t*>(score.data()) +
                                     score.size() * sizeof(float));
        scoreTensor.data = set.storage.back().data();
        scoreTensor.byteCount = set.storage.back().size();

        set.outputs.push_back(boxTensor);
        set.outputs.push_back(scoreTensor);
        (void)cells;
    }
};

vision::ModelContext squareContext(int side, int classes) {
    vision::ModelContext context;
    context.inputSize = core::Size{side, side};
    context.sourceSize = core::Size{side, side};
    // No letterbox: source and input are the same size, so the content fills
    // the input and mapping back is the identity. Keeps this test about the
    // decode rather than about the mapping, which core_tests already covers.
    context.contentRect = core::Rect{0, 0, side, side};
    context.confidence = 0.25f;
    (void)classes;
    return context;
}

AiStage stage(const std::string& type, int parent) {
    AiStage out;
    out.modelType = type;
    out.modelPath = "/models/whatever.rknn";
    out.parent = parent;
    return out;
}

}  // namespace

// --- the registries ----------------------------------------------------------

VS_TEST(model_types_register_themselves_rather_than_being_listed) {
    // The predecessor had an if/else chain naming every class AND a parallel
    // vector of names that had to be kept in step by hand. Adding a type meant
    // editing both plus the REST validation that read them.
    const auto types = vision::modelTypes();
    VS_CHECK(!types.empty());

    bool foundYolo = false;
    for (const vision::ModelType* type : types) {
        if (type->id() == "yolov8_detect") foundYolo = true;
    }
    VS_CHECK(foundYolo);
    VS_CHECK(vision::modelType("yolov8_detect") != nullptr);
    VS_CHECK(vision::modelType("no-such-type") == nullptr);
}

VS_TEST(the_plain_crop_is_a_registered_transform_not_a_special_case) {
    // So the runner asks for the transform named "" and gets it, rather than
    // branching on whether one was configured.
    VS_CHECK(vision::transform("") != nullptr);
    VS_CHECK(!vision::transforms().empty());
    VS_CHECK(vision::transform("no-such-transform") == nullptr);
}

// --- the stage tree ----------------------------------------------------------

VS_TEST(a_stage_tree_is_validated_before_anything_is_loaded) {
    // So a bad job is rejected when it is created, not when it fails to start
    // on a board at three in the morning.
    VS_CHECK(vision::validateStages({}).ok() == false);

    // The simple case.
    VS_CHECK(vision::validateStages({stage("yolov8_detect", -1)}).ok());

    // Stage zero runs on the whole frame and cannot have a parent.
    VS_CHECK(!vision::validateStages({stage("yolov8_detect", 0)}).ok());

    // A two-stage job: detect, then run something on each detection.
    VS_CHECK(vision::validateStages({stage("yolov8_detect", -1),
                                      stage("yolov8_detect", 0)})
                 .ok());

    // A parent must point BACKWARD. That one rule is what makes the tree
    // acyclic and lets it run in array order with no bookkeeping.
    VS_CHECK(!vision::validateStages({stage("yolov8_detect", -1),
                                       stage("yolov8_detect", 1)})
                 .ok());
    VS_CHECK(!vision::validateStages({stage("yolov8_detect", -1),
                                       stage("yolov8_detect", 5)})
                 .ok());

    // An unknown type is named in the error, because an operator has to fix it
    // from that message alone.
    const auto unknown = vision::validateStages({stage("no-such-model", -1)});
    VS_CHECK(!unknown.ok());
    VS_CHECK(unknown.error().message.find("no-such-model") != std::string::npos);
}

VS_TEST(a_tree_that_is_too_deep_or_too_wide_is_refused) {
    std::vector<AiStage> deep{stage("yolov8_detect", -1)};
    for (int i = 1; i < 8; ++i) deep.push_back(stage("yolov8_detect", i - 1));
    VS_CHECK(!vision::validateStages(deep).ok());

    std::vector<AiStage> wide{stage("yolov8_detect", -1)};
    for (int i = 0; i < 12; ++i) wide.push_back(stage("yolov8_detect", 0));
    VS_CHECK(!vision::validateStages(wide).ok());
}

VS_TEST(an_empty_class_filter_keeps_everything) {
    // What an operator who has not thought about classes yet expects.
    VS_CHECK(vision::passesFilter({}, 0));
    VS_CHECK(vision::passesFilter({}, 999));
    VS_CHECK(vision::passesFilter({2, 5}, 5));
    VS_CHECK(!vision::passesFilter({2, 5}, 3));
}

// --- the YOLOv8 decode -------------------------------------------------------

VS_TEST(one_object_in_one_cell_decodes_to_one_box) {
    hal::TensorSet tensors;
    HeadBuilder head(20, 20, /*classes=*/4);   // stride 32 for a 640 input
    // Cell (10, 10), class 2, comfortably over the threshold, four bins out on
    // every side.
    head.put(10, 10, /*classId=*/2, /*confidence=*/0.9f, /*sideBin=*/4);
    head.appendTo(tensors);

    const vision::ModelType* yolo = vision::modelType("yolov8_detect");
    VS_CHECK(yolo != nullptr);
    if (!yolo) return;

    auto decoded = yolo->decode(tensors, squareContext(640, 4));
    VS_CHECK(decoded.ok());
    if (!decoded.ok()) return;
    VS_CHECK_EQ(decoded.value().size(), std::size_t{1});
    if (decoded.value().empty()) return;

    const vision::Detection& detection = decoded.value().front();
    VS_CHECK_EQ(detection.classId, 2);
    VS_CHECK(std::fabs(detection.score - 0.9f) < 0.01f);

    // The cell centre is at (10.5, 10.5) * 32 = 336, and four bins at stride 32
    // is 128 in each direction.
    VS_CHECK(std::fabs(detection.x1 - (336.0f - 128.0f)) < 1.0f);
    VS_CHECK(std::fabs(detection.y1 - (336.0f - 128.0f)) < 1.0f);
    VS_CHECK(std::fabs(detection.x2 - (336.0f + 128.0f)) < 1.0f);
    VS_CHECK(std::fabs(detection.y2 - (336.0f + 128.0f)) < 1.0f);
}

VS_TEST(anything_under_the_confidence_threshold_is_dropped) {
    hal::TensorSet tensors;
    HeadBuilder head(20, 20, 4);
    head.put(5, 5, 1, /*confidence=*/0.10f, 4);   // below 0.25
    head.put(15, 15, 1, /*confidence=*/0.80f, 4); // above
    head.appendTo(tensors);

    auto decoded = vision::modelType("yolov8_detect")->decode(tensors, squareContext(640, 4));
    VS_CHECK(decoded.ok());
    if (decoded.ok()) VS_CHECK_EQ(decoded.value().size(), std::size_t{1});
}

VS_TEST(overlapping_boxes_of_one_class_are_suppressed_but_not_across_classes) {
    hal::TensorSet tensors;
    HeadBuilder head(20, 20, 4);
    // Two adjacent cells, same class, boxes big enough to overlap heavily.
    head.put(10, 10, /*classId=*/1, 0.9f, /*sideBin=*/6);
    head.put(11, 10, /*classId=*/1, 0.8f, /*sideBin=*/6);
    // A third in the same place but a DIFFERENT class. Suppressing across
    // classes would delete a person standing in front of a car, which is
    // exactly the case a VMS exists for.
    head.put(10, 11, /*classId=*/3, 0.85f, /*sideBin=*/6);
    head.appendTo(tensors);

    auto decoded = vision::modelType("yolov8_detect")->decode(tensors, squareContext(640, 4));
    VS_CHECK(decoded.ok());
    if (!decoded.ok()) return;

    int classOne = 0;
    int classThree = 0;
    for (const auto& detection : decoded.value()) {
        if (detection.classId == 1) ++classOne;
        if (detection.classId == 3) ++classThree;
    }
    VS_CHECK_EQ(classOne, 1);     // the weaker duplicate went
    VS_CHECK_EQ(classThree, 1);   // the other class stayed
}

VS_TEST(a_class_filter_is_applied_during_the_decode) {
    hal::TensorSet tensors;
    HeadBuilder head(20, 20, 4);
    head.put(4, 4, /*classId=*/0, 0.9f, 4);
    head.put(14, 14, /*classId=*/3, 0.9f, 4);
    head.appendTo(tensors);

    vision::ModelContext context = squareContext(640, 4);
    context.classFilter = {3};
    auto decoded = vision::modelType("yolov8_detect")->decode(tensors, context);
    VS_CHECK(decoded.ok());
    if (!decoded.ok()) return;
    VS_CHECK_EQ(decoded.value().size(), std::size_t{1});
    if (!decoded.value().empty()) VS_CHECK_EQ(decoded.value().front().classId, 3);
}

VS_TEST(tensors_that_are_not_a_yolov8_export_are_refused_with_a_reason) {
    // Rather than decoding rubbish into confident-looking boxes.
    hal::TensorSet empty;
    auto decoded = vision::modelType("yolov8_detect")->decode(empty, squareContext(640, 4));
    VS_CHECK(!decoded.ok());
    VS_CHECK(decoded.error().message.find("YOLOv8") != std::string::npos);
}

VS_TEST(several_scales_are_all_decoded) {
    // A real export has three: 80x80, 40x40 and 20x20 for a 640 input.
    hal::TensorSet tensors;
    HeadBuilder large(80, 80, 4);
    large.put(40, 40, 0, 0.9f, 3);
    large.appendTo(tensors);
    HeadBuilder medium(40, 40, 4);
    medium.put(5, 5, 1, 0.9f, 3);
    medium.appendTo(tensors);
    HeadBuilder small(20, 20, 4);
    small.put(2, 2, 2, 0.9f, 3);
    small.appendTo(tensors);

    auto decoded = vision::modelType("yolov8_detect")->decode(tensors, squareContext(640, 4));
    VS_CHECK(decoded.ok());
    if (decoded.ok()) VS_CHECK_EQ(decoded.value().size(), std::size_t{3});
}

VS_MAIN()
