#pragma once

// Keeps every camera that should be recording, recording.
//
// The counterpart of StreamManager, and deliberately its sibling rather than
// part of it: restreaming and recording start and stop for different reasons,
// and one failing must not take the other down. A camera whose disk is full
// should still be viewable.
//
// One worker thread drives all of it — settling the motion gate and expiring
// old segments — for the same reason StreamManager has one: a thread per camera
// is a hundred mostly-sleeping threads on a board with four cores.

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Result.hpp"
#include "media/camera/Camera.hpp"
#include "media/recording/MotionGate.hpp"
#include "media/recording/RecordingRepository.hpp"
#include "media/recording/RecordingSession.hpp"
#include "media/source/CameraSourceRegistry.hpp"

namespace visora::media {

struct RecordingManagerConfig {
    std::string recordingDir = "recordings";
    // How often the gate is settled and retention is applied. A minute: neither
    // deadline is urgent, and waking a board every second to find nothing to do
    // is a cost paid forever.
    int sweepSeconds = 60;
};

class RecordingManager {
public:
    RecordingManager(RecordingManagerConfig config,
                     std::shared_ptr<RecordingRepository> repository,
                     std::shared_ptr<CameraSourceRegistry> sources);
    ~RecordingManager();

    RecordingManager(const RecordingManager&) = delete;
    RecordingManager& operator=(const RecordingManager&) = delete;

    core::Status start();
    void stop();

    // Brings a camera under management, or updates one already there.
    //
    // `codec` comes from the streaming layer, which discovered it by probing.
    // Unknown means the camera is not up yet: recording waits rather than
    // guessing, because a pipeline built for the wrong codec fails in a way
    // that looks like a broken camera.
    void apply(const Camera& camera, Codec codec);

    void remove(const std::string& cameraId);

    // Something happened worth keeping footage of. In Motion mode this is what
    // saves the segments around it; in Continuous mode it does nothing, because
    // everything is being kept anyway.
    //
    // The source is not named on purpose: motion detection provides one and so
    // does an AI job that recognised something. Neither is a concept recording
    // needs to know about.
    void noteEvent(const std::string& cameraId);

    // Whether a camera is currently writing segments. For the status endpoint.
    bool isRecording(const std::string& cameraId) const;

private:
    struct Entry {
        Camera camera;
        Codec codec = Codec::Unknown;
        std::shared_ptr<EncodedSource> source;
        std::unique_ptr<RecordingSession> session;
        std::unique_ptr<MotionGate> gate;
        // What the running session was built for, so an unrelated edit does not
        // restart it and lose the segment being written.
        std::string builtSource;
        RecordingMode builtMode = RecordingMode::Off;
        int builtSegmentSeconds = 0;
    };

    void workerLoop();
    void onSegment(const std::string& cameraId, const RecordingSegment& segment);
    // Both return what should be deleted rather than deleting it. The caller
    // holds the lock for the first and must NOT hold it for the second: one is
    // arithmetic, the other queries the database.
    std::vector<RecordingSegment> settleGate(Entry& entry, std::int64_t nowMs);
    std::vector<RecordingSegment> expiredSegments(const std::string& cameraId,
                                                  int retentionDays, std::int64_t nowMs);
    // Deletes the rows and their files. Files first would leave rows pointing
    // at nothing; rows first would leave files nobody can find.
    void deleteSegments(const std::vector<RecordingSegment>& segments);
    static bool shouldRecord(const Camera& camera);

    RecordingManagerConfig m_config;
    std::shared_ptr<RecordingRepository> m_repository;
    std::shared_ptr<CameraSourceRegistry> m_sources;

    // Serialises apply() against itself, so it can release m_mutex to stop a
    // session without a second apply building a rival one. See apply().
    std::mutex m_applyMutex;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<std::string, Entry> m_entries;
    std::atomic<bool> m_running{false};
    std::thread m_worker;
};

// Whether a change to a camera means the recording pipeline has to be rebuilt.
// A rename must not interrupt a recording; a new segment length must.
bool recordingNeedsRestart(const Camera& camera, const std::string& builtSource,
                           RecordingMode builtMode, int builtSegmentSeconds);

}  // namespace visora::media
