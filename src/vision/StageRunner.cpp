#include "vision/StageRunner.hpp"

#include <algorithm>
#include <string>

#include "core/ImageMath.hpp"
#include "core/Log.hpp"
#include "hal/ImageOps.hpp"

namespace visora::vision {
namespace {

constexpr const char* kCategory = "vision";

// The fit mode each preparation means, so the image layer is asked in its own
// vocabulary rather than the model's.
core::FitMode fitModeFor(FramePrep prep) {
    switch (prep) {
        case FramePrep::Letterbox: return core::FitMode::Letterbox;
        case FramePrep::Stretch:   return core::FitMode::Stretch;
        case FramePrep::FitHeight: return core::FitMode::FitHeight;
    }
    return core::FitMode::Letterbox;
}

}  // namespace

bool passesFilter(const std::vector<int>& filter, int classId) {
    if (filter.empty()) return true;
    return std::find(filter.begin(), filter.end(), classId) != filter.end();
}

core::Status validateStages(const std::vector<AiStage>& stages) {
    if (stages.empty()) return core::invalidArgument("a job needs at least one stage");
    if (stages.size() > kMaxStages) {
        return core::invalidArgument("a job may have at most " + std::to_string(kMaxStages) +
                                     " stages");
    }

    std::vector<int> depth(stages.size(), 0);
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const AiStage& stage = stages[i];
        if (stage.modelType.empty()) {
            return core::invalidArgument("stage " + std::to_string(i) + " has no model type");
        }
        if (stage.modelPath.empty()) {
            return core::invalidArgument("stage " + std::to_string(i) + " has no model path");
        }
        if (!modelType(stage.modelType)) {
            return core::invalidArgument("stage " + std::to_string(i) + ": unknown model type '" +
                                         stage.modelType + "'");
        }
        if (!stage.transform.empty() && !transform(stage.transform)) {
            return core::invalidArgument("stage " + std::to_string(i) + ": unknown transform '" +
                                         stage.transform + "'");
        }

        if (i == 0) {
            if (stage.parent != -1) {
                return core::invalidArgument("stage 0 runs on the whole frame and cannot have "
                                             "a parent");
            }
            continue;
        }
        // A parent must point BACKWARD. That single rule is what makes the tree
        // acyclic and lets it be run in array order with no bookkeeping.
        if (stage.parent < 0 || static_cast<std::size_t>(stage.parent) >= i) {
            return core::invalidArgument("stage " + std::to_string(i) +
                                         " must name an earlier stage as its parent");
        }
        depth[i] = depth[stage.parent] + 1;
        if (depth[i] >= kMaxStageDepth) {
            return core::invalidArgument("stage " + std::to_string(i) + " nests deeper than " +
                                         std::to_string(kMaxStageDepth));
        }
    }
    return {};
}

core::Status StageRunner::init(const std::vector<AiStage>& stages, const std::string& tag) {
    m_stages.clear();
    m_tag = tag;

    const core::Status valid = validateStages(stages);
    if (!valid.ok()) return valid;

    m_stages.reserve(stages.size());
    for (std::size_t i = 0; i < stages.size(); ++i) {
        Stage stage;
        stage.config = stages[i];
        stage.type = modelType(stages[i].modelType);
        stage.transform = stages[i].transform.empty() ? transform("") 
                                                      : transform(stages[i].transform);

        auto model = hal::loadModel(hal::ModelRef{stages[i].modelPath});
        if (!model) {
            return core::Error{model.error().code,
                               tag + " stage " + std::to_string(i) + ": " +
                                   model.error().message};
        }
        stage.model = std::move(model.value());
        m_stages.push_back(std::move(stage));
    }

    for (std::size_t i = 1; i < m_stages.size(); ++i) {
        m_stages[static_cast<std::size_t>(m_stages[i].config.parent)].children.push_back(
            static_cast<int>(i));
    }

    VS_INFO(kCategory) << tag << ": " << m_stages.size() << " stage(s) loaded";
    return {};
}

core::Result<std::vector<Detection>> StageRunner::run(const core::ImageView& frame) {
    if (m_stages.empty()) return core::invalidArgument("the runner has no stages");

    Stage& root = m_stages[0];
    const core::Size inputSize = root.model->inputSize();

    // Fit the frame to the model. The image layer decides how — RGA on a
    // Rockchip board, OpenCV elsewhere — and this does not know which.
    auto ops = hal::imageOps();
    if (!ops) return ops.error();

    core::OwnedImage input(root.model->inputFormat(), inputSize);
    core::MutableImageView view = input.view();
    // The padding value matters for a letterbox: 114 is what YOLO is trained
    // with, and padding with black shifts what the model sees at the edges.
    auto content = ops.value()->fit(frame, view, fitModeFor(root.type->framePrep()), 114);
    if (!content) return content.error();

    auto tensors = root.model->run(view);
    if (!tensors) return tensors.error();

    ModelContext context;
    context.inputSize = inputSize;
    context.sourceSize = frame.size;
    // Taken from fit() rather than recomputed: the backend that placed the
    // content is the one that knows where it put it, and an accelerator with an
    // alignment constraint does not always land where the arithmetic says.
    context.contentRect = content.value();
    context.confidence = root.config.confidence;
    context.classFilter = root.config.classFilter;

    auto detections = root.type->decode(tensors.value(), context);
    if (!detections) return detections.error();

    std::vector<Detection> kept;
    for (Detection& detection : detections.value()) {
        if (!passesFilter(root.config.classFilter, detection.classId)) continue;
        // Stage zero's boxes are already in frame coordinates, but recording
        // them here means every later stage reads ONE field regardless of how
        // deep it sits.
        detection.fx1 = detection.x1;
        detection.fy1 = detection.y1;
        detection.fx2 = detection.x2;
        detection.fy2 = detection.y2;
        detection.hasFrameBox = true;
        detection.stage = 0;
        kept.push_back(std::move(detection));
    }

    for (const int child : root.children) {
        const core::Status ran =
            runChildStage(static_cast<std::size_t>(child), frame, kept);
        if (!ran.ok()) return ran.error();
    }
    return kept;
}

core::Status StageRunner::runChildStage(std::size_t index, const core::ImageView& frame,
                                        std::vector<Detection>& parents) {
    Stage& stage = m_stages[index];
    const core::Size inputSize = stage.model->inputSize();

    for (Detection& parent : parents) {
        // The INPUT filter: which of the parent's classes this stage runs on.
        if (!passesFilter(stage.config.inputClasses, parent.classId)) continue;

        TransformContext transformContext;
        transformContext.source = frame;
        transformContext.detection = &parent;
        // Frame coordinates, always — see the header.
        transformContext.box = core::Rect{static_cast<int>(parent.fx1),
                                          static_cast<int>(parent.fy1),
                                          static_cast<int>(parent.fx2 - parent.fx1),
                                          static_cast<int>(parent.fy2 - parent.fy1)};
        transformContext.keypoints = &parent.keypoints;
        transformContext.target = inputSize;
        transformContext.prep = stage.type->framePrep();
        transformContext.tightCrop = stage.type->prefersTightCrop();

        core::OwnedImage cropped(stage.model->inputFormat(), inputSize);
        core::MutableImageView cropView = cropped.view();
        const core::Status applied = stage.transform->apply(transformContext, cropView);
        if (!applied.ok()) {
            // Unsupported means "skip this detection" — a face transform with
            // no landmarks, say. The rest of the frame still goes through.
            if (applied.error().code == core::ErrorCode::Unsupported) continue;
            return applied;
        }

        auto tensors = stage.model->run(cropView);
        if (!tensors) return tensors.error();

        ModelContext context;
        context.inputSize = inputSize;
        // What the crop covered, so child boxes map back to the frame.
        const core::Rect covered = transformContext.covered.width > 0
                                       ? transformContext.covered
                                       : transformContext.box;
        context.sourceSize = core::Size{covered.width, covered.height};
        context.contentRect = transformContext.contentRect.width > 0
                                  ? transformContext.contentRect
                                  : core::fitContentRect(fitModeFor(stage.type->framePrep()),
                                                          context.sourceSize, inputSize);
        context.confidence = stage.config.confidence;
        context.classFilter = stage.config.classFilter;

        const std::size_t before = parent.children.size();
        const core::Status enriched = stage.type->enrich(tensors.value(), context, parent);
        if (!enriched.ok()) return enriched;

        // Give every new child its frame-space box and stage number, so a
        // THIRD stage can crop from it and so a consumer can order children
        // from several crops in one coordinate system.
        for (std::size_t i = before; i < parent.children.size(); ++i) {
            Detection& child = parent.children[i];
            if (!passesFilter(stage.config.classFilter, child.classId)) continue;
            child.fx1 = static_cast<float>(covered.x) + child.x1;
            child.fy1 = static_cast<float>(covered.y) + child.y1;
            child.fx2 = static_cast<float>(covered.x) + child.x2;
            child.fy2 = static_cast<float>(covered.y) + child.y2;
            child.hasFrameBox = true;
            child.stage = static_cast<int>(index);
        }

        // Drop the ones the output filter rejected, after mapping so the loop
        // above stays simple.
        parent.children.erase(
            std::remove_if(parent.children.begin() + static_cast<long>(before),
                           parent.children.end(),
                           [&stage](const Detection& child) {
                               return !passesFilter(stage.config.classFilter, child.classId);
                           }),
            parent.children.end());

        for (const int grandchild : stage.children) {
            const core::Status ran = runChildStage(static_cast<std::size_t>(grandchild), frame,
                                                   parent.children);
            if (!ran.ok()) return ran;
        }
    }
    return {};
}

}  // namespace visora::vision
