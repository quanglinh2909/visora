#pragma once

// AI job business rules.
//
// Validation happens HERE, not when a job fails to start: a stage tree with a
// bad parent or an unknown model type is rejected at creation, while the person
// who wrote it is still looking at it.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "vision/AiJob.hpp"
#include "vision/AiJobRepository.hpp"

namespace visora::vision {

// What the service tells the world. The AI runtime subscribes, the same way
// the streaming layer subscribes to camera events.
struct AiJobEvents {
    std::function<void(const AiJob&)> added;
    std::function<void(const AiJob&, const AiJobDiff&)> changed;
    std::function<void(const std::string& id)> removed;
};

class AiJobService {
public:
    explicit AiJobService(std::shared_ptr<AiJobRepository> repository,
                          AiJobEvents events = {});

    core::Result<std::vector<AiJob>> list();
    core::Result<std::vector<AiJob>> listForCamera(const std::string& cameraId);
    core::Result<AiJob> get(const std::string& id);

    core::Result<AiJob> create(const AiJobChanges& changes);
    core::Result<AiJob> update(const std::string& id, const AiJobChanges& changes);
    core::Status remove(const std::string& id);

    // Start and stop are just `enabled`, so a job that is stopped stays stopped
    // across a restart. A separate runtime-only flag would not.
    core::Result<AiJob> setEnabled(const std::string& id, bool enabled);

private:
    std::shared_ptr<AiJobRepository> m_repository;
    AiJobEvents m_events;
};

core::Status validateForCreate(const AiJobChanges& changes);
core::Status validateForUpdate(const AiJobChanges& changes);

}  // namespace visora::vision
