#pragma once

// Runs the AI jobs.
//
// One decoder per CAMERA (FrameTap), one worker per JOB. That split is the
// point: three jobs on one camera decode once between them, and a job that
// takes 80 ms to run does not hold up the camera's other jobs or its decoder.
//
// Each worker keeps only the LATEST frame. A worker slower than the frame rate
// skips, because analysing a frame from four seconds ago is worth nothing and
// queueing them turns a busy moment into unbounded memory.

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Result.hpp"
#include "media/ai/FrameTap.hpp"
#include "media/ai/JpegEncoder.hpp"
#include "media/camera/Camera.hpp"
#include "media/source/CameraSourceRegistry.hpp"
#include "vision/AiJob.hpp"
#include "vision/ResultSink.hpp"
#include "vision/StageRunner.hpp"

namespace visora::media {

struct AiRuntimeConfig {
    // How often a camera is decoded for AI. Not every frame: a detector at 5
    // fps sees everything that matters and leaves the accelerator for the other
    // cameras.
    int analyseFps = 5;
    int jpegQuality = 85;
};

// What a job is doing, for the status endpoint.
struct AiJobStatus {
    std::string jobId;
    std::string cameraId;
    bool running = false;
    std::string lastError;
    std::uint64_t framesAnalysed = 0;
    std::uint64_t resultsPublished = 0;
    // Milliseconds the last inference took. The number that tells an operator
    // whether the accelerator is keeping up.
    double lastInferenceMs = 0.0;
};

class AiRuntime {
public:
    AiRuntime(AiRuntimeConfig config, std::shared_ptr<CameraSourceRegistry> sources,
              std::shared_ptr<vision::ResultSinkSet> sinks);
    ~AiRuntime();

    AiRuntime(const AiRuntime&) = delete;
    AiRuntime& operator=(const AiRuntime&) = delete;

    core::Status start();
    void stop();

    // A camera's stream changed: its codec is now known, or it went away.
    // Jobs on it are (re)started or stopped accordingly.
    void applyCamera(const Camera& camera, Codec codec);

    // Brings a job under management, or updates one already there.
    void applyJob(const vision::AiJob& job);
    void removeJob(const std::string& jobId);

    std::vector<AiJobStatus> statuses() const;

    // Something worth keeping footage of happened. Wired to the recording
    // manager, so an AI detection can trigger event-based recording.
    using EventSink = std::function<void(const std::string& cameraId)>;
    void setEventSink(EventSink sink) { m_onEvent = std::move(sink); }

private:
    struct Worker;
    struct CameraEntry;

    // The tap for a camera, created on demand and dropped when its last job
    // goes. Held weakly for the same reason the source registry does: nothing
    // has to remember to release it.
    std::shared_ptr<FrameTap> tapFor(const std::string& cameraId);
    void restartJob(const std::string& jobId);

    AiRuntimeConfig m_config;
    std::shared_ptr<CameraSourceRegistry> m_sources;
    std::shared_ptr<vision::ResultSinkSet> m_sinks;
    EventSink m_onEvent;

    mutable std::mutex m_mutex;
    std::map<std::string, CameraEntry> m_cameras;
    std::map<std::string, std::shared_ptr<Worker>> m_workers;
    std::atomic<bool> m_running{false};
};

}  // namespace visora::media
