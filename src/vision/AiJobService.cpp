#include "vision/AiJobService.hpp"

#include <utility>

#include "core/Log.hpp"
#include "vision/StageRunner.hpp"

namespace visora::vision {
namespace {

constexpr const char* kCategory = "vision";
constexpr std::size_t kMaxNameLength = 128;

core::Status validateCommon(const AiJobChanges& changes) {
    if (changes.name.has_value()) {
        if (changes.name->empty()) return core::invalidArgument("a job needs a name");
        if (changes.name->size() > kMaxNameLength) {
            return core::invalidArgument("the name is too long");
        }
    }
    if (changes.maxFps.has_value() && (*changes.maxFps < 0 || *changes.maxFps > 120)) {
        return core::invalidArgument("maxFps must be between 0 (unlimited) and 120");
    }
    // The stage tree is checked in full here, so a bad job is refused while the
    // person who wrote it is still looking at it — not at three in the morning
    // when a board restarts and the job will not load.
    if (changes.stages.has_value()) {
        const core::Status stages = validateStages(*changes.stages);
        if (!stages.ok()) return stages;
    }
    return {};
}

}  // namespace

core::Status validateForCreate(const AiJobChanges& changes) {
    if (!changes.name.has_value()) return core::invalidArgument("a job needs a name");
    if (!changes.cameraId.has_value() || changes.cameraId->empty()) {
        return core::invalidArgument("a job needs a camera");
    }
    if (!changes.stages.has_value()) return core::invalidArgument("a job needs stages");
    return validateCommon(changes);
}

core::Status validateForUpdate(const AiJobChanges& changes) {
    if (changes.cameraId.has_value() && changes.cameraId->empty()) {
        return core::invalidArgument("a job needs a camera");
    }
    return validateCommon(changes);
}

AiJobService::AiJobService(std::shared_ptr<AiJobRepository> repository, AiJobEvents events)
    : m_repository(std::move(repository)), m_events(std::move(events)) {}

core::Result<std::vector<AiJob>> AiJobService::list() { return m_repository->list(); }

core::Result<std::vector<AiJob>> AiJobService::listForCamera(const std::string& cameraId) {
    return m_repository->listForCamera(cameraId);
}

core::Result<AiJob> AiJobService::get(const std::string& id) { return m_repository->get(id); }

core::Result<AiJob> AiJobService::create(const AiJobChanges& changes) {
    const core::Status valid = validateForCreate(changes);
    if (!valid.ok()) return valid.error();

    AiJob job;
    apply(changes, job);

    auto stored = m_repository->insert(job);
    if (!stored) return stored.error();

    VS_INFO(kCategory) << "created job " << stored.value().id << " (" << stored.value().name
                       << ") on camera " << stored.value().cameraId;
    if (m_events.added) m_events.added(stored.value());
    return stored;
}

core::Result<AiJob> AiJobService::update(const std::string& id, const AiJobChanges& changes) {
    const core::Status valid = validateForUpdate(changes);
    if (!valid.ok()) return valid.error();

    auto existing = m_repository->get(id);
    if (!existing) return existing.error();

    AiJob job = existing.value();
    const AiJobDiff diff = apply(changes, job);

    auto stored = m_repository->update(job);
    if (!stored) return stored.error();

    if (!diff.cosmeticOnly() && m_events.changed) m_events.changed(stored.value(), diff);
    return stored;
}

core::Status AiJobService::remove(const std::string& id) {
    const core::Status removed = m_repository->remove(id);
    if (!removed.ok()) return removed;
    if (m_events.removed) m_events.removed(id);
    return {};
}

core::Result<AiJob> AiJobService::setEnabled(const std::string& id, bool enabled) {
    AiJobChanges changes;
    changes.enabled = enabled;
    return update(id, changes);
}

}  // namespace visora::vision
