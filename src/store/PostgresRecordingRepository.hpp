#pragma once

// The PostgreSQL RecordingRepository.
//
// Column names and types are the predecessor's, unchanged: an existing
// deployment's recordings must keep working, and the files on disk are indexed
// by these rows alone.

#include <memory>
#include <string>
#include <vector>

#include "media/recording/RecordingRepository.hpp"

namespace oatpp::orm {
class Executor;
}

namespace visora::store {

class PostgresRecordingRepository final : public media::RecordingRepository {
public:
    explicit PostgresRecordingRepository(std::shared_ptr<oatpp::orm::Executor> executor);
    ~PostgresRecordingRepository();

    core::Result<media::RecordingSegment> upsertSegment(
        const media::RecordingSegment& segment) override;
    core::Result<std::vector<media::RecordingSegment>> segmentsInRange(
        const std::string& cameraId, std::int64_t fromMs, std::int64_t toMs) override;
    core::Result<media::RecordingSegment> segment(const std::string& id) override;
    core::Result<std::vector<media::RecordingSegment>> segmentsEndingBefore(
        const std::string& cameraId, std::int64_t beforeMs) override;
    core::Status removeSegments(const std::vector<std::string>& ids) override;

    core::Result<media::MotionEvent> insertMotionEvent(const media::MotionEvent& event) override;
    core::Status closeMotionEvent(const std::string& id, std::int64_t endMs, double maxScore,
                                  const std::string& cells) override;
    core::Result<std::vector<media::MotionEvent>> motionEventsInRange(
        const std::string& cameraId, std::int64_t fromMs, std::int64_t toMs) override;
    core::Result<media::MotionEvent> motionEvent(const std::string& id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::store
