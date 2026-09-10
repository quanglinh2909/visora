// The vision layer: model-type and transform registries, stage-tree validation,
// and the YOLOv8 decode.
//
// The decode is asserted against tensors built here rather than against a model
// file, so it runs on a machine with no NPU, no ONNX Runtime and no weights —
// which is the point of abstracting inference at the tensor level.

#include "TestHarness.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "vision/ModelType.hpp"
#include "vision/MotionDetector.hpp"
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
    // So the runner asks for a transform and gets one, rather than branching on
    // whether a stage configured it.
    VS_CHECK(!vision::transforms().empty());
    VS_CHECK(vision::transform("no-such-transform") == nullptr);
}

VS_TEST(the_crop_transform_has_a_name_a_client_can_use) {
    // It used to register as "", which listed an entry in GET /ai-transforms
    // that no caller could refer to — found on the board.
    bool named = false;
    for (const vision::Transform* item : vision::transforms()) {
        if (item->id() == "crop") named = true;
        // Nothing may list itself without a name.
        VS_CHECK(!item->id().empty());
    }
    VS_CHECK(named);
    VS_CHECK(vision::transform("crop") != nullptr);

    // An empty id still resolves to it, so a stage that omits the field works —
    // and so does every job stored before the crop had a name.
    VS_CHECK(vision::transform("") == vision::transform("crop"));
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


// --- the pose, segmentation, OCR and embedding decodes ------------------------
//
// Same principle as the YOLOv8 tests above: tensors built here, so the maths is
// asserted on a machine with no NPU, no ONNX Runtime and no weights. These are
// the decodes it is least possible to eyeball — a pose skeleton read from the
// wrong anchor still looks like a skeleton.

namespace {

// Appends a float tensor, copying the data into the set's own storage.
void appendFloat(hal::TensorSet& set, const char* name, std::vector<int> shape,
                 const std::vector<float>& values) {
    hal::Tensor tensor;
    tensor.name = name;
    tensor.type = hal::TensorType::Float32;
    tensor.shape = std::move(shape);
    const auto* begin = reinterpret_cast<const std::uint8_t*>(values.data());
    set.storage.emplace_back(begin, begin + values.size() * sizeof(float));
    tensor.data = set.storage.back().data();
    tensor.byteCount = set.storage.back().size();
    set.outputs.push_back(std::move(tensor));
}

// One pose branch: 64 DFL channels then ONE score channel, which is a logit.
struct PoseBranch {
    int grid = 0;
    int bins = 16;
    std::vector<float> data;

    explicit PoseBranch(int g) : grid(g) {
        data.assign(static_cast<std::size_t>(65 * g * g), 0.0f);
        // Background is a large NEGATIVE logit, which is what a trained pose
        // head emits and what the decode is written for. Leaving it at zero
        // would make every cell an 0.5-confidence person, because the score
        // channel here is a logit rather than the probability the detector
        // head emits — the one real difference between the two decodes.
        for (int cell = 0; cell < g * g; ++cell) {
            data[static_cast<std::size_t>(64 * g * g + cell)] = -10.0f;
        }
    }

    void put(int cellX, int cellY, float logit, int sideBin) {
        const int cells = grid * grid;
        const int cell = cellY * grid + cellX;
        data[static_cast<std::size_t>(64 * cells + cell)] = logit;
        for (int side = 0; side < 4; ++side) {
            data[static_cast<std::size_t>((side * bins + sideBin) * cells + cell)] = 20.0f;
        }
    }

    void appendTo(hal::TensorSet& set) const {
        appendFloat(set, "branch", {1, 65, grid, grid}, data);
    }
};

}  // namespace

VS_TEST(pose_reads_each_skeleton_from_the_anchor_its_box_came_from) {
    // The keypoint tensor is one flat array over every anchor of every scale,
    // and suppression sorts by score — so without the anchor index a surviving
    // box would be given somebody else's joints. That failure looks like a
    // plausible skeleton in the wrong place, which is why it is asserted.
    hal::TensorSet tensors;
    PoseBranch branch(20);                        // stride 32 for a 640 input
    branch.put(10, 10, /*logit=*/4.0f, /*sideBin=*/4);
    branch.appendTo(tensors);

    constexpr int kJoints = 17;
    const int anchors = 20 * 20;
    std::vector<float> joints(static_cast<std::size_t>(kJoints * 3 * anchors), 0.0f);
    const int anchor = 10 * 20 + 10;
    // Joint 0 of THAT anchor, in model-input pixels.
    joints[static_cast<std::size_t>(0 * 3 * anchors + anchor)] = 300.0f;              // x
    joints[static_cast<std::size_t>(0 * 3 * anchors + anchors + anchor)] = 350.0f;    // y
    joints[static_cast<std::size_t>(0 * 3 * anchors + 2 * anchors + anchor)] = 0.9f;  // score
    appendFloat(tensors, "keypoints", {1, kJoints, 3, anchors}, joints);

    const vision::ModelType* pose = vision::modelType("yolov8_pose");
    VS_CHECK(pose != nullptr);
    if (!pose) return;

    auto decoded = pose->decode(tensors, squareContext(640, 1));
    VS_CHECK(decoded.ok());
    if (!decoded.ok() || decoded.value().empty()) return;
    VS_CHECK_EQ(decoded.value().size(), std::size_t{1});

    const vision::Detection& detection = decoded.value().front();
    VS_CHECK_EQ(detection.classId, 0);
    // sigmoid(4) is about 0.982 — the score channel is a logit here, where the
    // detector's is already a probability.
    VS_CHECK(std::fabs(detection.score - 0.982f) < 0.01f);
    VS_CHECK(std::fabs(detection.x1 - (336.0f - 128.0f)) < 1.0f);

    VS_CHECK_EQ(detection.keypoints.size(), std::size_t{kJoints * 3});
    if (detection.keypoints.size() < 3) return;
    VS_CHECK(std::fabs(detection.keypoints[0] - 300.0f) < 1.0f);
    VS_CHECK(std::fabs(detection.keypoints[1] - 350.0f) < 1.0f);
    VS_CHECK(std::fabs(detection.keypoints[2] - 0.9f) < 0.01f);
}

VS_TEST(pose_refuses_outputs_whose_anchor_counts_do_not_line_up) {
    // A skipped or reordered scale means every joint is read from the wrong
    // anchor. Silence there would be worse than an error, because the result
    // still looks like a person.
    hal::TensorSet tensors;
    PoseBranch branch(20);
    branch.put(10, 10, 4.0f, 4);
    branch.appendTo(tensors);
    // The keypoint tensor claims three scales' worth of anchors.
    appendFloat(tensors, "keypoints", {1, 17, 3, 8400},
                std::vector<float>(static_cast<std::size_t>(17 * 3 * 8400), 0.0f));

    auto decoded = vision::modelType("yolov8_pose")->decode(tensors, squareContext(640, 1));
    VS_CHECK(!decoded.ok());
    VS_CHECK(decoded.error().message.find("anchors") != std::string::npos);
}

VS_TEST(segmentation_paints_a_silhouette_inside_the_box_it_belongs_to) {
    // Three scales of (box, score, coefficients) plus the proto basis.
    hal::TensorSet tensors;
    constexpr int kInput = 160;
    constexpr int kCoefficients = 4;
    const int grids[3] = {20, 10, 5};

    for (int scale = 0; scale < 3; ++scale) {
        const int grid = grids[scale];
        const int cells = grid * grid;
        std::vector<float> box(static_cast<std::size_t>(64 * cells), 0.0f);
        std::vector<float> score(static_cast<std::size_t>(1 * cells), 0.0f);
        std::vector<float> coefficients(static_cast<std::size_t>(kCoefficients * cells), 0.0f);

        if (scale == 0) {
            // Cell (10, 10) at stride 8: centre 84, four bins out is 32.
            const int cell = 10 * grid + 10;
            score[static_cast<std::size_t>(cell)] = 0.9f;
            for (int side = 0; side < 4; ++side) {
                box[static_cast<std::size_t>((side * 16 + 4) * cells + cell)] = 20.0f;
            }
            // Only basis 0 contributes.
            coefficients[static_cast<std::size_t>(0 * cells + cell)] = 1.0f;
        }

        appendFloat(tensors, "box", {1, 64, grid, grid}, box);
        appendFloat(tensors, "score", {1, 1, grid, grid}, score);
        appendFloat(tensors, "coeff", {1, kCoefficients, grid, grid}, coefficients);
    }

    // The proto: basis 0 positive on the LEFT half of the input, negative on
    // the right. So the silhouette must fill the left half of the box and
    // nothing else.
    constexpr int kProto = 40;   // a quarter of the input, as a real export is
    std::vector<float> proto(static_cast<std::size_t>(kCoefficients * kProto * kProto), -1.0f);
    for (int y = 0; y < kProto; ++y) {
        for (int x = 0; x < kProto / 2; ++x) {
            proto[static_cast<std::size_t>(y) * kProto + x] = 1.0f;
        }
    }
    appendFloat(tensors, "proto", {1, kCoefficients, kProto, kProto}, proto);

    vision::ModelContext context;
    context.inputSize = core::Size{kInput, kInput};
    context.sourceSize = core::Size{kInput, kInput};
    context.contentRect = core::Rect{0, 0, kInput, kInput};
    context.confidence = 0.25f;

    const vision::ModelType* seg = vision::modelType("yolov8_seg");
    VS_CHECK(seg != nullptr);
    if (!seg) return;

    auto decoded = seg->decode(tensors, context);
    VS_CHECK(decoded.ok());
    if (!decoded.ok() || decoded.value().empty()) return;
    VS_CHECK_EQ(decoded.value().size(), std::size_t{1});

    const vision::Detection& detection = decoded.value().front();
    // 128 bytes: a 32x32 bitmap, which is what the wire format carries.
    VS_CHECK_EQ(detection.maskBits.size(), std::size_t{128});
    if (detection.maskBits.size() != 128) return;

    // The box spans 52..116, and the proto is positive below x = 80. So the
    // left 44% of the box's width is set and the rest is clear.
    constexpr int kGrid = vision::Detection::kMaskGrid;
    const auto bitAt = [&detection](int gx, int gy) {
        const int bit = gy * kGrid + gx;
        return (detection.maskBits[static_cast<std::size_t>(bit >> 3)] >> (bit & 7)) & 1u;
    };
    VS_CHECK(bitAt(0, 16) == 1u);
    VS_CHECK(bitAt(5, 16) == 1u);
    VS_CHECK(bitAt(kGrid - 1, 16) == 0u);
    VS_CHECK(bitAt(kGrid - 2, 0) == 0u);
}

VS_TEST(segmentation_sends_no_mask_rather_than_an_empty_one) {
    // 128 zero bytes would say "an empty silhouette" where the truth is "no
    // silhouette" — and a viewer cannot tell those apart.
    hal::TensorSet tensors;
    constexpr int kInput = 160;
    const int grids[3] = {20, 10, 5};
    for (int scale = 0; scale < 3; ++scale) {
        const int grid = grids[scale];
        const int cells = grid * grid;
        std::vector<float> box(static_cast<std::size_t>(64 * cells), 0.0f);
        std::vector<float> score(static_cast<std::size_t>(cells), 0.0f);
        std::vector<float> coefficients(static_cast<std::size_t>(4 * cells), 0.0f);
        if (scale == 0) {
            const int cell = 10 * grid + 10;
            score[static_cast<std::size_t>(cell)] = 0.9f;
            for (int side = 0; side < 4; ++side) {
                box[static_cast<std::size_t>((side * 16 + 4) * cells + cell)] = 20.0f;
            }
            coefficients[static_cast<std::size_t>(cell)] = 1.0f;
        }
        appendFloat(tensors, "box", {1, 64, grid, grid}, box);
        appendFloat(tensors, "score", {1, 1, grid, grid}, score);
        appendFloat(tensors, "coeff", {1, 4, grid, grid}, coefficients);
    }
    // Every basis negative everywhere: nothing belongs to the object.
    appendFloat(tensors, "proto", {1, 4, 40, 40},
                std::vector<float>(static_cast<std::size_t>(4 * 40 * 40), -1.0f));

    vision::ModelContext context;
    context.inputSize = core::Size{kInput, kInput};
    context.sourceSize = core::Size{kInput, kInput};
    context.contentRect = core::Rect{0, 0, kInput, kInput};
    context.confidence = 0.25f;

    auto decoded = vision::modelType("yolov8_seg")->decode(tensors, context);
    VS_CHECK(decoded.ok());
    if (!decoded.ok() || decoded.value().empty()) return;
    VS_CHECK(decoded.value().front().maskBits.empty());
}

VS_TEST(text_detection_merges_the_fragments_of_one_line_into_one_box) {
    // DBNet outputs SHRUNK character regions, so a small or thin-stroked image
    // gives one blob per character. A 128x110 plate measured 20 fragments, and
    // handing each to the recogniser produced nothing at all.
    constexpr int kMap = 40;
    std::vector<float> map(static_cast<std::size_t>(kMap * kMap), 0.0f);
    const auto fill = [&map](int x1, int y1, int x2, int y2) {
        for (int y = y1; y < y2; ++y) {
            for (int x = x1; x < x2; ++x) map[static_cast<std::size_t>(y) * kMap + x] = 1.0f;
        }
    };
    // Three "characters" on one line, a pixel apart.
    fill(5, 10, 9, 18);
    fill(11, 10, 15, 18);
    fill(17, 10, 21, 18);

    hal::TensorSet tensors;
    appendFloat(tensors, "prob", {1, 1, kMap, kMap}, map);

    vision::ModelContext context;
    context.inputSize = core::Size{160, 160};
    context.sourceSize = core::Size{160, 160};
    context.contentRect = core::Rect{0, 0, 160, 160};
    context.confidence = 0.25f;

    const vision::ModelType* det = vision::modelType("paddle_ocr_det");
    VS_CHECK(det != nullptr);
    if (!det) return;

    auto decoded = det->decode(tensors, context);
    VS_CHECK(decoded.ok());
    if (!decoded.ok()) return;
    VS_CHECK_EQ(decoded.value().size(), std::size_t{1});
    if (decoded.value().empty()) return;

    // One box covering all three, at four times the map scale.
    const vision::Detection& line = decoded.value().front();
    VS_CHECK(line.x1 < 5.0f * 4.0f);
    VS_CHECK(line.x2 > 20.0f * 4.0f);
    VS_CHECK(line.score > 0.9f);
}

VS_TEST(text_detection_does_not_let_a_tall_stroke_swallow_the_line_beside_it) {
    // A plate border or a table rule is tall and thin. Merging it with the text
    // it encloses produced a single box covering the whole 128x110 picture.
    constexpr int kMap = 40;
    std::vector<float> map(static_cast<std::size_t>(kMap * kMap), 0.0f);
    const auto fill = [&map](int x1, int y1, int x2, int y2) {
        for (int y = y1; y < y2; ++y) {
            for (int x = x1; x < x2; ++x) map[static_cast<std::size_t>(y) * kMap + x] = 1.0f;
        }
    };
    fill(4, 2, 8, 36);     // the border: 34 tall, 4 wide
    fill(12, 16, 20, 22);  // the text: 6 tall, well inside it

    hal::TensorSet tensors;
    appendFloat(tensors, "prob", {1, 1, kMap, kMap}, map);

    vision::ModelContext context;
    context.inputSize = core::Size{160, 160};
    context.sourceSize = core::Size{160, 160};
    context.contentRect = core::Rect{0, 0, 160, 160};
    context.confidence = 0.25f;

    auto decoded = vision::modelType("paddle_ocr_det")->decode(tensors, context);
    VS_CHECK(decoded.ok());
    if (!decoded.ok()) return;
    VS_CHECK_EQ(decoded.value().size(), std::size_t{2});
}

VS_TEST(text_recognition_collapses_ctc_blanks_and_repeats_into_characters) {
    // The head emits one distribution per timestep; the same character held for
    // three steps is one character, and the blank class is not one at all.
    constexpr int kSteps = 8;
    constexpr int kClasses = 5;   // blank + four dictionary entries
    std::vector<float> head(static_cast<std::size_t>(kSteps * kClasses), 0.0f);
    const auto emit = [&head](int step, int classId, float value) {
        head[static_cast<std::size_t>(step) * kClasses + classId] = value;
    };
    emit(0, 0, 0.9f);   // blank
    emit(1, 1, 0.9f);   // 'A'
    emit(2, 1, 0.9f);   // 'A' again — the same character still being emitted
    emit(3, 0, 0.9f);   // blank
    emit(4, 1, 0.9f);   // 'A' again, but AFTER a blank, so a second one
    emit(5, 2, 0.9f);   // 'B'
    emit(6, 0, 0.9f);   // blank
    emit(7, 3, 0.9f);   // 'C'

    hal::TensorSet tensors;
    appendFloat(tensors, "ctc", {1, kSteps, kClasses}, head);

    vision::ModelContext context;
    context.inputSize = core::Size{320, 48};
    context.sourceSize = core::Size{320, 48};
    context.contentRect = core::Rect{0, 0, 320, 48};
    context.confidence = 0.25f;

    const vision::ModelType* rec = vision::modelType("paddle_ocr_rec");
    VS_CHECK(rec != nullptr);
    if (!rec) return;

    auto decoded = rec->decode(tensors, context);
    VS_CHECK(decoded.ok());
    if (!decoded.ok()) return;
    VS_CHECK_EQ(decoded.value().size(), std::size_t{4});
    if (decoded.value().size() != 4) return;

    // Class ids are dictionary rows, so the blank is already subtracted.
    VS_CHECK_EQ(decoded.value()[0].classId, 0);
    VS_CHECK_EQ(decoded.value()[1].classId, 0);
    VS_CHECK_EQ(decoded.value()[2].classId, 1);
    VS_CHECK_EQ(decoded.value()[3].classId, 2);

    // The x of each character is its timestep, which is what lets a plate
    // assembler sort them and split a two-line plate.
    VS_CHECK(decoded.value()[0].x1 < decoded.value()[2].x1);
    VS_CHECK(decoded.value()[2].x1 < decoded.value()[3].x1);
}

VS_TEST(text_recognition_reads_its_dictionary_from_beside_the_model) {
    // Not compiled in: a recogniser trained on Vietnamese plates and one
    // trained on Chinese receipts are the same code and different sidecars.
    const std::string model = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                              "/visora_ppocr_test.rknn";
    const std::string sidecar = model.substr(0, model.size() - 5) + ".txt";
    {
        std::ofstream out(sidecar);
        out << "X\nY\nZ\n";
    }

    constexpr int kSteps = 3;
    constexpr int kClasses = 4;
    std::vector<float> head(static_cast<std::size_t>(kSteps * kClasses), 0.0f);
    head[0 * kClasses + 1] = 0.9f;   // dictionary row 0 -> "X"
    head[1 * kClasses + 3] = 0.9f;   // dictionary row 2 -> "Z"
    head[2 * kClasses + 0] = 0.9f;   // blank

    hal::TensorSet tensors;
    appendFloat(tensors, "ctc", {1, kSteps, kClasses}, head);

    vision::ModelContext context;
    context.inputSize = core::Size{320, 48};
    context.sourceSize = core::Size{320, 48};
    context.contentRect = core::Rect{0, 0, 320, 48};
    context.confidence = 0.25f;
    context.modelPath = model;

    auto decoded = vision::modelType("paddle_ocr_rec")->decode(tensors, context);
    VS_CHECK(decoded.ok());
    if (!decoded.ok() || decoded.value().size() != 2) return;
    VS_CHECK(decoded.value()[0].text == "X");
    VS_CHECK(decoded.value()[1].text == "Z");
    std::remove(sidecar.c_str());
}

VS_TEST(a_face_embedding_enriches_its_parent_and_detects_nothing) {
    // It is not a detector, and says so by implementing only enrich(). Used as
    // stage zero it produces nothing, which is the honest answer.
    const vision::ModelType* face = vision::modelType("face_recognition");
    VS_CHECK(face != nullptr);
    if (!face) return;

    std::vector<float> vector(512);
    for (std::size_t i = 0; i < vector.size(); ++i) {
        vector[i] = static_cast<float>(i) * 0.001f;
    }
    hal::TensorSet tensors;
    appendFloat(tensors, "embedding", {1, 512}, vector);

    vision::ModelContext context = squareContext(112, 1);

    auto asDetector = face->decode(tensors, context);
    VS_CHECK(asDetector.ok());
    if (asDetector.ok()) VS_CHECK(asDetector.value().empty());

    vision::Detection parent;
    const core::Status enriched = face->enrich(tensors, context, parent);
    VS_CHECK(enriched.ok());
    VS_CHECK_EQ(parent.embedding.size(), std::size_t{512});
    if (parent.embedding.size() == 512) {
        // Raw, as the network produced it: whoever compares two faces
        // normalises as part of the cosine similarity, and doing it here as
        // well would be either redundant or silently wrong.
        VS_CHECK(std::fabs(parent.embedding[100] - 0.1f) < 1e-4f);
    }
    VS_CHECK(parent.children.empty());
}

// --- motion ------------------------------------------------------------------

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A packed RGB frame filled with one grey, so a change can be introduced
// exactly where the test wants it.
struct TestFrame {
    int width, height;
    std::vector<std::uint8_t> pixels;

    TestFrame(int w, int h, std::uint8_t value) : width(w), height(h) {
        pixels.assign(static_cast<std::size_t>(w) * h * 3, value);
    }

    void fillRect(int x, int y, int w, int h, std::uint8_t value) {
        for (int row = y; row < y + h && row < height; ++row) {
            for (int column = x; column < x + w && column < width; ++column) {
                const std::size_t at = (static_cast<std::size_t>(row) * width + column) * 3;
                pixels[at] = pixels[at + 1] = pixels[at + 2] = value;
            }
        }
    }

    core::ImageView view() const {
        return core::ImageView::packed(core::PixelFormat::RGB888,
                                       core::Size{width, height}, pixels.data());
    }
};

}  // namespace

VS_TEST(the_first_frame_can_only_be_recorded_not_judged) {
    // There is nothing to compare it against, and reporting motion for it would
    // make every camera fire the moment it starts.
    vision::MotionDetector detector({8, 8});
    TestFrame frame(320, 240, 100);
    VS_CHECK(!detector.primed());
    VS_CHECK(detector.analyse(frame.view(), {}).empty());
    VS_CHECK(detector.primed());
}

VS_TEST(a_still_scene_reports_nothing) {
    vision::MotionDetector detector({8, 8});
    TestFrame frame(320, 240, 100);
    detector.analyse(frame.view(), {});
    VS_CHECK(detector.analyse(frame.view(), {}).empty());
}

VS_TEST(sensor_noise_below_the_threshold_is_not_motion) {
    // Every camera has it, and without a floor the whole grid lights up at
    // night — which is how a motion feature becomes one nobody leaves on.
    vision::MotionDetector detector({8, 8});
    TestFrame first(320, 240, 100);
    detector.analyse(first.view(), {});

    TestFrame second(320, 240, 100 + vision::kMotionPixelDelta - 2);
    VS_CHECK(detector.analyse(second.view(), {}).empty());
}

VS_TEST(something_moving_reports_the_cells_it_moved_in) {
    vision::MotionDetector detector({8, 8});
    TestFrame first(320, 240, 100);
    detector.analyse(first.view(), {});

    // A bright block in the top-left eighth of the frame: cell 0:0.
    TestFrame second(320, 240, 100);
    second.fillRect(0, 0, 40, 30, 255);
    const std::string cells = detector.analyse(second.view(), {});
    VS_CHECK(!cells.empty());
    VS_CHECK(contains(cells, "0:0"));
    // And not the far corner.
    VS_CHECK(!contains(cells, "7:7"));
}

VS_TEST(the_grid_is_laid_over_the_picture_not_over_the_letterbox_padding) {
    // Otherwise the edge cells never move, and the zones an operator drew on
    // the picture sit over the wrong cells.
    vision::MotionDetector detector({4, 4});

    // A 320x240 picture inside a 320x320 frame, padding above and below.
    TestFrame first(320, 320, 0);
    first.fillRect(0, 40, 320, 240, 100);
    detector.analyse(first.view(), core::Rect{0, 40, 320, 240});

    // Something moves at the very TOP of the picture, which is y=40 in the
    // frame. With the grid over the picture that is row 0.
    TestFrame second(320, 320, 0);
    second.fillRect(0, 40, 320, 240, 100);
    second.fillRect(0, 40, 80, 60, 255);
    const std::string cells = detector.analyse(second.view(), core::Rect{0, 40, 320, 240});
    VS_CHECK(contains(cells, "0:0"));
}

VS_TEST(nv12_is_read_straight_from_the_y_plane) {
    // What a hardware decoder produces, and the Y plane already IS luminance —
    // so this is the format that costs nothing. The predecessor spent 15% of a
    // core per 1080p camera converting frames for its motion branch.
    const int width = 320;
    const int height = 240;
    const std::size_t ySize = static_cast<std::size_t>(width) * height;

    std::vector<std::uint8_t> first(ySize + ySize / 2, 128);
    std::vector<std::uint8_t> second = first;
    // A bright block in the top-left eighth of the Y plane.
    for (int row = 0; row < 30; ++row) {
        for (int column = 0; column < 40; ++column) {
            second[static_cast<std::size_t>(row) * width + column] = 255;
        }
    }

    const auto viewOf = [&](const std::vector<std::uint8_t>& bytes) {
        return core::ImageView::packed(core::PixelFormat::NV12,
                                       core::Size{width, height}, bytes.data());
    };

    vision::MotionDetector detector({8, 8});
    VS_CHECK(detector.analyse(viewOf(first), {}).empty());
    const std::string cells = detector.analyse(viewOf(second), {});
    VS_CHECK(contains(cells, "0:0"));
    VS_CHECK(!contains(cells, "7:7"));
}

VS_TEST(a_camera_with_no_zones_never_fires) {
    // Deliberate. The predecessor measured 11 of 12 cameras with empty zones,
    // every one running the whole motion branch and discarding every result.
    VS_CHECK(!vision::zonesTriggered("0:0,0:1,1:0,1:1", {}, {8, 8}));
}

VS_TEST(a_zones_level_is_relative_to_the_zone_not_to_the_frame) {
    // So a small zone stays as sensitive as a large one.
    vision::MotionGrid grid{10, 10};

    // A zone of 2x2 = 4 cells at level 5, so half its cells must move.
    std::vector<vision::MotionZone> zone{{0, 0, 1, 1, 5}};
    VS_CHECK(!vision::zonesTriggered("0:0", zone, grid));         // 1 of 4
    VS_CHECK(vision::zonesTriggered("0:0,0:1", zone, grid));      // 2 of 4

    // Motion outside the zone does not count, however much of it there is.
    VS_CHECK(!vision::zonesTriggered("9:9,8:8,7:7,6:6", zone, grid));
}

VS_TEST(zone_json_that_cannot_be_read_makes_a_camera_quiet_not_noisy) {
    const auto good = vision::parseMotionZones(
        R"([{"r1":1,"c1":2,"r2":3,"c2":4,"level":7}])");
    VS_CHECK_EQ(good.size(), std::size_t{1});
    if (!good.empty()) {
        VS_CHECK_EQ(good[0].row1, 1);
        VS_CHECK_EQ(good[0].col2, 4);
        VS_CHECK_EQ(good[0].level, 7);
    }

    // Reversed corners are normalised rather than producing an empty zone.
    const auto reversed = vision::parseMotionZones(R"([{"r1":5,"c1":5,"r2":1,"c2":1}])");
    VS_CHECK_EQ(reversed.size(), std::size_t{1});
    if (!reversed.empty()) VS_CHECK_EQ(reversed[0].row1, 1);

    // Nothing readable means no zones, which means the camera does not fire —
    // the safe direction.
    VS_CHECK(vision::parseMotionZones("").empty());
    VS_CHECK(vision::parseMotionZones("not json at all").empty());
    VS_CHECK(vision::parseMotionZones("[]").empty());
}

// --- alignment transforms ----------------------------------------------------

namespace {

// The canonical ArcFace landmark positions for a 112x112 crop: the published
// 96x112 numbers with the horizontal centring every reference implementation
// applies. Written out here rather than shared with the transform on purpose —
// a test that imports the constant it is checking proves nothing.
constexpr float kCanonX[5] = {38.2946f, 73.5318f, 56.0252f, 41.5493f, 70.7299f};
constexpr float kCanonY[5] = {51.6963f, 51.5014f, 71.7366f, 92.3655f, 92.2041f};

// Five distinguishable colours, one per landmark, so the check is "landmark 3
// arrived at position 3" and not merely "something bright arrived".
constexpr std::uint8_t kDotColour[5][3] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255},
};

struct FaceScene {
    core::OwnedImage frame;
    std::vector<float> keypoints;  // flat (x, y, score), frame coordinates
    core::Rect box;
};

// Paints a synthetic face: the canonical landmarks rotated by `degrees`, scaled
// and moved into a blank frame, each marked with its own colour. Aligning this
// must undo exactly that rotation.
FaceScene paintFace(float degrees, float scale, float originX, float originY) {
    constexpr int kFrameSide = 480;
    constexpr int kDotRadius = 4;

    FaceScene scene;
    scene.frame.reset(core::PixelFormat::RGB888, core::Size{kFrameSide, kFrameSide});
    scene.frame.fill(0);

    const float radians = degrees * 3.14159265f / 180.0f;
    const float cosine = std::cos(radians) * scale;
    const float sine = std::sin(radians) * scale;

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    core::MutableImageView view = scene.frame.view();
    for (int i = 0; i < 5; ++i) {
        const float x = originX + cosine * kCanonX[i] - sine * kCanonY[i];
        const float y = originY + sine * kCanonX[i] + cosine * kCanonY[i];
        scene.keypoints.push_back(x);
        scene.keypoints.push_back(y);
        scene.keypoints.push_back(0.9f);
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);

        for (int dy = -kDotRadius; dy <= kDotRadius; ++dy) {
            for (int dx = -kDotRadius; dx <= kDotRadius; ++dx) {
                const int px = static_cast<int>(x) + dx;
                const int py = static_cast<int>(y) + dy;
                if (px < 0 || py < 0 || px >= kFrameSide || py >= kFrameSide) continue;
                std::uint8_t* pixel = view.data +
                                      static_cast<std::size_t>(py) * view.planes[0].stride +
                                      static_cast<std::size_t>(px) * 3;
                pixel[0] = kDotColour[i][0];
                pixel[1] = kDotColour[i][1];
                pixel[2] = kDotColour[i][2];
            }
        }
    }

    // The box a face detector would report: the landmarks with a little around
    // them, which is less than the canonical square reaches.
    scene.box = core::Rect{static_cast<int>(minX) - 10, static_cast<int>(minY) - 10,
                           static_cast<int>(maxX - minX) + 20,
                           static_cast<int>(maxY - minY) + 20};
    return scene;
}

// Runs one transform over a scene and hands back the 112x112 RGB result.
core::OwnedImage alignFace(const FaceScene& scene, vision::TransformContext* seen = nullptr) {
    vision::TransformContext context;
    context.source = scene.frame.view();
    context.box = scene.box;
    context.keypoints = &scene.keypoints;
    context.target = core::Size{112, 112};

    core::OwnedImage out(core::PixelFormat::RGB888, context.target);
    core::MutableImageView view = out.view();
    const core::Status applied = vision::transform("align_face")->apply(context, view);
    if (!applied.ok()) return core::OwnedImage{};
    if (seen != nullptr) *seen = context;
    return out;
}

const std::uint8_t* pixelAt(const core::OwnedImage& image, int x, int y) {
    return image.data() + static_cast<std::size_t>(y) * image.size().width * 3 +
           static_cast<std::size_t>(x) * 3;
}

// Which of the five colours a pixel is closest to, or -1 when it is nearer to
// the black background than to any of them.
int nearestDot(const std::uint8_t* pixel) {
    int best = -1;
    int bestDistance = 3 * 128 * 128;  // must beat "closer to black than to a dot"
    for (int i = 0; i < 5; ++i) {
        int distance = 0;
        for (int c = 0; c < 3; ++c) {
            const int delta = static_cast<int>(pixel[c]) - static_cast<int>(kDotColour[i][c]);
            distance += delta * delta;
        }
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

}  // namespace

VS_TEST(the_alignment_transforms_the_stored_jobs_name_are_registered) {
    // Live jobs on the board carry these two ids. Before they existed here,
    // every such job failed validation with "unknown transform" — the job would
    // not start, and the REST call that saved it returned 400.
    VS_CHECK(vision::transform("align_face") != nullptr);
    VS_CHECK(vision::transform("align_plate") != nullptr);

    bool face = false;
    bool plate = false;
    for (const vision::Transform* item : vision::transforms()) {
        if (item->id() == "align_face") face = true;
        if (item->id() == "align_plate") plate = true;
    }
    VS_CHECK(face && plate);
}

VS_TEST(face_alignment_puts_the_landmarks_where_the_recogniser_expects_them) {
    // The whole contract of this transform: after it runs, the left eye is at
    // the canonical left-eye position, and so on. A rotated, scaled, offset
    // face goes in; the canonical layout comes out.
    const FaceScene scene = paintFace(/*degrees=*/22.0f, /*scale=*/1.7f, 150.0f, 120.0f);
    const core::OwnedImage aligned = alignFace(scene);
    VS_CHECK(!aligned.empty());

    for (int i = 0; i < 5; ++i) {
        const int x = static_cast<int>(kCanonX[i] + 0.5f);
        const int y = static_cast<int>(kCanonY[i] + 0.5f);
        // Landmark i, and not one of the other four: a transform that mirrored
        // the face or swapped the eyes would still land colour ON a landmark.
        VS_CHECK_EQ(nearestDot(pixelAt(aligned, x, y)), i);
    }
}

VS_TEST(face_alignment_removes_the_pose_rather_than_merely_cropping) {
    // The same face at two rotations must produce the SAME aligned image. This
    // is what the embedding model is promised and what a plain crop cannot do:
    // crop the two and they differ by the rotation.
    const core::OwnedImage upright = alignFace(paintFace(0.0f, 1.7f, 150.0f, 150.0f));
    const core::OwnedImage tilted = alignFace(paintFace(35.0f, 1.2f, 190.0f, 90.0f));
    VS_CHECK(!upright.empty() && !tilted.empty());
    VS_CHECK_EQ(upright.byteCount(), tilted.byteCount());

    double total = 0;
    for (std::size_t i = 0; i < upright.byteCount(); ++i) {
        total += std::abs(static_cast<int>(upright.data()[i]) -
                          static_cast<int>(tilted.data()[i]));
    }
    const double meanDifference = total / static_cast<double>(upright.byteCount());
    // Not zero: two different rotations resample different pixels, so dot edges
    // differ. Small, because everything else is made to coincide.
    VS_CHECK(meanDifference < 6.0);
}

VS_TEST(face_alignment_declines_a_detection_with_no_landmarks) {
    // Unsupported means SKIP THIS DETECTION, not "the job failed" — a box
    // detector used by mistake, or a face too small to resolve joints on,
    // must not take down the rest of the frame.
    FaceScene scene = paintFace(0.0f, 1.7f, 150.0f, 150.0f);
    scene.keypoints.clear();

    vision::TransformContext context;
    context.source = scene.frame.view();
    context.box = scene.box;
    context.keypoints = &scene.keypoints;
    context.target = core::Size{112, 112};

    core::OwnedImage out(core::PixelFormat::RGB888, context.target);
    core::MutableImageView view = out.view();
    const core::Status applied = vision::transform("align_face")->apply(context, view);
    VS_CHECK(!applied.ok());
    VS_CHECK(applied.error().code == core::ErrorCode::Unsupported);
}

VS_TEST(plate_alignment_crops_tight_where_the_plain_crop_pads) {
    // The one thing that separates them, and the reason align_plate is not just
    // an alias: context padding shrinks the characters inside a fixed model
    // input, which is the margin on a distant plate.
    core::OwnedImage frame(core::PixelFormat::RGB888, core::Size{640, 480});
    frame.fill(80);
    const core::Rect box{200, 200, 120, 40};

    auto coveredBy = [&](const char* id) {
        vision::TransformContext context;
        context.source = frame.view();
        context.box = box;
        context.target = core::Size{192, 48};
        core::OwnedImage out(core::PixelFormat::RGB888, context.target);
        core::MutableImageView view = out.view();
        VS_CHECK(vision::transform(id)->apply(context, view).ok());
        return context.covered;
    };

    const core::Rect tight = coveredBy("align_plate");
    const core::Rect padded = coveredBy("crop");
    VS_CHECK(tight == box);
    VS_CHECK(padded.width > box.width && padded.height > box.height);
}


VS_MAIN()
