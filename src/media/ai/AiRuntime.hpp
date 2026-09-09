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
#include "media/ai/JpegDecoder.hpp"
#include "media/ai/JpegEncoder.hpp"
#include "media/camera/Camera.hpp"
#include "media/source/CameraSourceRegistry.hpp"
#include "vision/AiJob.hpp"
#include "vision/MotionDetector.hpp"
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

// A motion event as it happens, for the websocket and the recording index.
struct MotionNotice {
    std::string cameraId;
    // Cells that moved, "row:col" comma-separated — the format a UI draws back.
    std::string cells;

    // The same cells split by whether a drawn zone covers them. Split HERE,
    // where the zones are, rather than in the websocket layer which has no way
    // to know them.
    //
    // Both are sent to a viewer, drawn differently. Showing only what is inside
    // hides exactly what an operator needs when a zone is in the wrong place:
    // something IS moving and the camera is saying nothing about it.
    std::string insideCells;
    std::string outsideCells;

    int gridX = 0;
    int gridY = 0;
    // True when a zone's level was reached. Cells move constantly; an EVENT is
    // what an operator asked to be told about.
    bool triggered = false;
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

    // Runs a stage tree over ONE uploaded image, with no camera involved.
    //
    // The point of it: an operator choosing a model, a confidence or a class
    // filter can try the thing on a picture and see what comes back, instead of
    // pointing a job at a camera and waiting for something to walk past.
    //
    // The SAME StageRunner as the live path, so what it shows is what a job
    // would do — a separate implementation would eventually disagree with the
    // one that matters.
    core::Result<std::vector<vision::Detection>> runOnce(
        const std::vector<vision::AiStage>& stages, const std::uint8_t* jpeg,
        std::size_t size);

    // Something worth keeping footage of happened. Wired to the recording
    // manager, so an AI detection can trigger event-based recording.
    using EventSink = std::function<void(const std::string& cameraId)>;
    void setEventSink(EventSink sink) { m_onEvent = std::move(sink); }

    // Every analysed frame's motion, whether or not it triggered. The empty
    // answer matters as much as a full one: it is what closes an open event.
    using MotionSink = std::function<void(const MotionNotice&)>;
    void setMotionSink(MotionSink sink) { m_onMotion = std::move(sink); }

private:
    struct Worker;

    // A camera's decoder, plus the motion detection that rides on the frames it
    // is already producing. Motion costs one subtraction per sampled point
    // BECAUSE the frame is already decoded and already in a format whose Y
    // plane is luminance — the predecessor paid 25% of a core per camera for
    // the same thing on a branch of its own.
    struct CameraEntry {
        Camera camera;
        Codec codec = Codec::Unknown;
        std::shared_ptr<FrameTap> tap;
        std::shared_ptr<vision::MotionDetector> motion;
        std::vector<vision::MotionZone> zones;
        std::uint64_t motionSinkId = 0;
    };

    // The tap for a camera, created on demand and dropped when its last job
    // goes. Held weakly for the same reason the source registry does: nothing
    // has to remember to release it.
    std::shared_ptr<FrameTap> tapFor(const std::string& cameraId);
    void restartJob(const std::string& jobId);
    // Attaches or detaches motion detection for a camera. Caller holds the lock.
    void updateMotion(CameraEntry& entry);

    AiRuntimeConfig m_config;
    std::shared_ptr<CameraSourceRegistry> m_sources;
    std::shared_ptr<vision::ResultSinkSet> m_sinks;
    EventSink m_onEvent;
    MotionSink m_onMotion;

    mutable std::mutex m_mutex;
    std::map<std::string, CameraEntry> m_cameras;
    std::map<std::string, std::shared_ptr<Worker>> m_workers;
    std::atomic<bool> m_running{false};
};

}  // namespace visora::media
