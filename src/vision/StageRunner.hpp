#pragma once

// Runs a job's stage tree over one frame.
//
// The ONLY place that knows how to run more than one stage — the live camera
// pipeline and the try-an-image endpoint both come here, so they cannot drift.
//
// WHY EVERY STAGE CROPS FROM THE ORIGINAL FRAME: from the third stage on, the
// box a parent reports is in ITS parent's crop space. Cropping a crop compounds
// the loss and lands in the wrong place. So each detection also carries its box
// in FRAME coordinates (Detection::fx1..fy2) and every stage cuts from the
// full-resolution frame using that.

#include <memory>
#include <string>
#include <vector>

#include "core/Image.hpp"
#include "core/Result.hpp"
#include "hal/InferenceBackend.hpp"
#include "vision/AiJob.hpp"
#include "vision/Detection.hpp"
#include "vision/ModelType.hpp"
#include "vision/Transform.hpp"

namespace visora::vision {

// Bounds that stop a nonsensical configuration without constraining a real one.
inline constexpr int kMaxStages = 8;
inline constexpr int kMaxStageDepth = 6;

class StageRunner {
public:
    // Loads every stage's model and resolves its transform. The error names the
    // stage and what was wrong with it, because a job that will not start is
    // something an operator has to fix from a message.
    core::Status init(const std::vector<AiStage>& stages, const std::string& tag);

    // Runs the whole tree. `frame` is the full-resolution source; the runner
    // fits it to each model itself.
    core::Result<std::vector<Detection>> run(const core::ImageView& frame);

    bool ready() const { return !m_stages.empty(); }
    std::size_t stageCount() const { return m_stages.size(); }

private:
    struct Stage {
        AiStage config;
        ModelType* type = nullptr;
        Transform* transform = nullptr;
        std::unique_ptr<hal::Model> model;
        std::vector<int> children;  // indices of stages whose parent is this one
    };

    // Runs `stage` over every detection `parent` produced and kept.
    core::Status runChildStage(std::size_t index, const core::ImageView& frame,
                               std::vector<Detection>& parents);

    std::vector<Stage> m_stages;
    std::string m_tag;
};

// Whether a detection's class passes a filter. Empty keeps everything, which is
// what an operator who has not thought about classes yet expects.
bool passesFilter(const std::vector<int>& filter, int classId);

// Validates a stage tree without loading anything: every parent points
// BACKWARD to an existing stage, stage zero is the only root, and the tree is
// neither too wide nor too deep. Separated so the REST layer rejects a bad job
// at creation rather than when it fails to start.
core::Status validateStages(const std::vector<AiStage>& stages);

}  // namespace visora::vision
