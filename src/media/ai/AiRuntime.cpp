#include "media/ai/AiRuntime.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "core/Log.hpp"
#include "core/Time.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "ai";

// Splits the moved cells by whether any drawn zone covers them.
void splitByZones(const std::string& cells, const std::vector<vision::MotionZone>& zones,
                  std::string& inside, std::string& outside) {
    std::size_t at = 0;
    while (at <= cells.size()) {
        const auto comma = cells.find(',', at);
        const std::string token =
            cells.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!token.empty()) {
            const auto colon = token.find(':');
            if (colon != std::string::npos) {
                const int row = std::atoi(token.c_str());
                const int column = std::atoi(token.c_str() + colon + 1);
                bool covered = false;
                for (const vision::MotionZone& zone : zones) {
                    if (row >= zone.row1 && row <= zone.row2 && column >= zone.col1 &&
                        column <= zone.col2) {
                        covered = true;
                        break;
                    }
                }
                std::string& target = covered ? inside : outside;
                if (!target.empty()) target += ',';
                target += token;
            }
        }
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
}

}  // namespace

// One job: a thread, the latest frame, and the stage tree to run on it.
struct AiRuntime::Worker {
    vision::AiJob job;
    std::shared_ptr<FrameTap> tap;
    std::uint64_t tapSinkId = 0;
    vision::StageRunner runner;
    JpegEncoder jpeg;
    std::shared_ptr<vision::ResultSinkSet> sinks;
    AiRuntime::EventSink onEvent;

    // The latest frame, COPIED. The decoder's buffer is valid only for the
    // duration of its callback, and the worker runs later on its own thread.
    std::mutex mutex;
    std::condition_variable wake;
    core::OwnedImage pending;
    std::int64_t pendingTsUs = 0;
    bool hasPending = false;

    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> framesAnalysed{0};
    std::atomic<std::uint64_t> resultsPublished{0};
    std::atomic<std::uint64_t> sequence{0};
    // Published as a double through an atomic of the same width: the status
    // endpoint reads it from another thread.
    std::atomic<double> lastInferenceMs{0.0};
    std::string lastError;
    mutable std::mutex errorMutex;
    std::thread thread;

    explicit Worker(int jpegQuality) : jpeg(jpegQuality) {}

    void note(const std::string& message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError = message;
    }

    std::string error() const {
        std::lock_guard<std::mutex> lock(errorMutex);
        return lastError;
    }

    void offer(const core::ImageView& frame, std::int64_t tsUs) {
        std::lock_guard<std::mutex> lock(mutex);
        // The LATEST frame replaces whatever was waiting. A worker slower than
        // the camera skips rather than falling behind — see the header.
        pending.reset(frame.format, frame.size);
        copyInto(frame, pending);
        pendingTsUs = tsUs;
        hasPending = true;
        wake.notify_one();
    }

    // Copies row by row: a hardware decoder pads rows to its own alignment, and
    // a straight memcpy of the plane would embed that padding as image data.
    static void copyInto(const core::ImageView& from, core::OwnedImage& to) {
        const core::Planes packed = core::Planes::packed(from.format, from.size);
        const int planes = core::planeCount(from.format);
        for (int p = 0; p < planes; ++p) {
            const int rows = p == 0 ? from.size.height : from.size.height / 2;
            const int width = core::packedStride(from.format, from.size.width, p);
            const int stride = from.planes[p].stride > 0 ? from.planes[p].stride : width;
            for (int row = 0; row < rows; ++row) {
                std::memcpy(to.data() + packed[p].offset +
                                static_cast<std::size_t>(row) * width,
                            from.data + from.planes[p].offset +
                                static_cast<std::size_t>(row) * stride,
                            static_cast<std::size_t>(width));
            }
        }
    }

    void loop() {
        VS_INFO(kCategory) << "job " << job.id << " started on camera " << job.cameraId;
        while (running.load()) {
            core::OwnedImage frame;
            std::int64_t tsUs = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(200),
                              [this] { return hasPending || !running.load(); });
                if (!running.load()) break;
                if (!hasPending) continue;
                frame = std::move(pending);
                tsUs = pendingTsUs;
                hasPending = false;
            }

            const auto began = std::chrono::steady_clock::now();
            auto detections = runner.run(frame.view());
            const auto took = std::chrono::steady_clock::now() - began;
            lastInferenceMs.store(
                std::chrono::duration<double, std::milli>(took).count());
            framesAnalysed.fetch_add(1);

            if (!detections) {
                note(detections.error().message);
                VS_WARN(kCategory) << "job " << job.id << ": " << detections.error().message;
                // Keep going. A single bad frame — a decoder hiccup, a model
                // that dislikes one input — must not end the job.
                continue;
            }
            note({});
            if (detections.value().empty()) continue;

            vision::Result result;
            result.cameraId = job.cameraId;
            result.jobId = job.id;
            result.seq = sequence.fetch_add(1);
            result.tsUs = tsUs;
            result.origWidth = frame.size().width;
            result.origHeight = frame.size().height;
            result.detections = std::move(detections.value());

            // The JPEG is most of the cost of producing a result, so it is only
            // encoded when something is actually listening.
            if (sinks && sinks->anyConsumers()) {
                auto encoded = jpeg.encode(frame.view());
                if (encoded) {
                    result.fullJpeg = std::move(encoded.value());
                } else {
                    VS_DEBUG(kCategory) << "job " << job.id
                                        << ": no JPEG: " << encoded.error().message;
                }
                sinks->publish(result);
                resultsPublished.fetch_add(1);
            }

            // Anything detected is worth keeping footage of, in a camera set to
            // record only around events. Recording decides what to do with it.
            if (onEvent) onEvent(job.cameraId);
        }
        VS_INFO(kCategory) << "job " << job.id << " stopped";
    }
};

AiRuntime::AiRuntime(AiRuntimeConfig config, std::shared_ptr<CameraSourceRegistry> sources,
                     std::shared_ptr<vision::ResultSinkSet> sinks)
    : m_config(config), m_sources(std::move(sources)), m_sinks(std::move(sinks)) {}

AiRuntime::~AiRuntime() { stop(); }

core::Status AiRuntime::start() {
    m_running.store(true);
    return {};
}

void AiRuntime::stop() {
    if (!m_running.exchange(false)) return;

    std::map<std::string, std::shared_ptr<Worker>> workers;
    std::map<std::string, CameraEntry> cameras;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        workers.swap(m_workers);
        cameras.swap(m_cameras);
    }
    for (auto& [id, worker] : workers) {
        worker->running.store(false);
        worker->wake.notify_all();
        if (worker->tap && worker->tapSinkId != 0) worker->tap->removeSink(worker->tapSinkId);
        if (worker->thread.joinable()) worker->thread.join();
        worker->jpeg.stop();
    }
    for (auto& [id, entry] : cameras) {
        if (entry.tap) entry.tap->stop();
    }
}

std::shared_ptr<FrameTap> AiRuntime::tapFor(const std::string& cameraId) {
    // Caller holds the lock.
    const auto it = m_cameras.find(cameraId);
    if (it == m_cameras.end()) return nullptr;
    CameraEntry& entry = it->second;
    if (entry.tap && entry.tap->running()) return entry.tap;
    if (entry.codec == Codec::Unknown) return nullptr;

    auto source = m_sources->acquire(cameraId, entry.camera.inputRtsp, entry.codec);
    if (!source) {
        VS_WARN(kCategory) << cameraId << ": " << source.error().message;
        return nullptr;
    }

    FrameTapOptions options;
    options.maxFps = m_config.analyseFps;
    auto tap = std::make_shared<FrameTap>(cameraId, source.value(), options);
    const core::Status started = tap->start();
    if (!started.ok()) {
        VS_WARN(kCategory) << cameraId << ": " << started.error().message;
        return nullptr;
    }
    entry.tap = tap;
    return tap;
}

void AiRuntime::updateMotion(CameraEntry& entry) {
    // Caller holds the lock.
    const bool wanted = entry.camera.motionEnabled && entry.tap && entry.tap->running();

    if (!wanted) {
        if (entry.tap && entry.motionSinkId != 0) entry.tap->removeSink(entry.motionSinkId);
        entry.motionSinkId = 0;
        entry.motion.reset();
        return;
    }
    if (entry.motionSinkId != 0) return;  // already attached

    vision::MotionGrid grid;
    grid.columns = entry.camera.motionGridX;
    grid.rows = entry.camera.motionGridY;
    entry.motion = std::make_shared<vision::MotionDetector>(grid);
    entry.zones = vision::parseMotionZones(entry.camera.motionZones);

    const std::string cameraId = entry.camera.id;
    auto motion = entry.motion;
    const auto zones = entry.zones;
    auto onMotion = m_onMotion;
    auto onEvent = m_onEvent;

    // How long an event stays open after the last frame that triggered it.
    //
    // Without this, one continuously moving object produces a burst of
    // start/end pairs: measured on a moving ball, ten of them in twelve
    // seconds, because between frames it does not always shift enough cells to
    // reach the level. An operator means one event, and the post-motion setting
    // they already configured is exactly the right length — it is what they
    // asked to keep recording for.
    const std::int64_t holdMs =
        static_cast<std::int64_t>(std::max(0, entry.camera.postMotionSeconds)) * 1000;
    auto lastTriggerMs = std::make_shared<std::atomic<std::int64_t>>(0);

    entry.motionSinkId = entry.tap->addSink(
        [cameraId, motion, zones, grid, onMotion, onEvent, holdMs,
         lastTriggerMs](const core::ImageView& frame, std::int64_t) {
            // On the decoder's thread, and cheap enough to belong there: one
            // subtraction per sampled point over a fixed 160x120 grid.
            const std::string cells = motion->analyse(frame, {});
            MotionNotice notice;
            notice.cameraId = cameraId;
            notice.cells = cells;
            notice.gridX = grid.columns;
            notice.gridY = grid.rows;
            const bool firing = vision::zonesTriggered(cells, zones, grid);
            const std::int64_t nowMs = core::nowEpochMs();
            if (firing) lastTriggerMs->store(nowMs);
            const std::int64_t since = lastTriggerMs->load();
            notice.triggered = firing || (since != 0 && nowMs - since < holdMs);
            splitByZones(cells, zones, notice.insideCells, notice.outsideCells);

            if (onMotion) onMotion(notice);
            // Only a triggering FRAME feeds the recording gate, not the whole
            // held window: the gate has its own pre- and post-roll, and telling
            // it repeatedly for one event would be noise.
            if (firing && onEvent) onEvent(cameraId);
        });

    VS_INFO(kCategory) << cameraId << ": motion detection on (" << grid.columns << 'x'
                       << grid.rows << " grid, " << zones.size() << " zone(s), " << holdMs
                       << "ms hold)";
}

void AiRuntime::applyCamera(const Camera& camera, Codec codec) {
    std::vector<std::string> toRestart;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CameraEntry& entry = m_cameras[camera.id];
        const bool changed = entry.codec != codec || entry.camera.inputRtsp != camera.inputRtsp;
        const bool motionChanged = entry.camera.motionEnabled != camera.motionEnabled ||
                                   entry.camera.motionZones != camera.motionZones ||
                                   entry.camera.motionGridX != camera.motionGridX ||
                                   entry.camera.motionGridY != camera.motionGridY;
        entry.camera = camera;
        entry.codec = codec;

        if (!changed) {
            if (motionChanged) {
                // Rebuild the detector for the new grid or zones, without
                // touching the decoder or any job.
                if (entry.tap && entry.motionSinkId != 0) {
                    entry.tap->removeSink(entry.motionSinkId);
                }
                entry.motionSinkId = 0;
                entry.motion.reset();
                updateMotion(entry);
            }
            // Motion alone can need a decoder that no job asked for.
            if (camera.motionEnabled && !entry.tap && codec != Codec::Unknown) {
                if (tapFor(camera.id)) updateMotion(entry);
            }
            return;
        }

        // The decoder was built for the old stream. Drop it; the next job that
        // needs one builds it again.
        if (entry.tap) {
            entry.tap->stop();
            entry.tap.reset();
        }
        entry.motionSinkId = 0;
        entry.motion.reset();
        for (const auto& [jobId, worker] : m_workers) {
            if (worker->job.cameraId == camera.id) toRestart.push_back(jobId);
        }
    }
    for (const std::string& jobId : toRestart) restartJob(jobId);

    // Motion may need a decoder even with no jobs at all: a camera can be set
    // to record on motion without anything analysing it.
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_cameras.find(camera.id);
    if (it == m_cameras.end()) return;
    if (it->second.camera.motionEnabled && codec != Codec::Unknown) {
        if (tapFor(camera.id)) updateMotion(it->second);
    }
}

void AiRuntime::applyJob(const vision::AiJob& job) {
    removeJob(job.id);
    if (!job.enabled) return;
    if (!m_running.load()) return;

    auto worker = std::make_shared<Worker>(m_config.jpegQuality);
    worker->job = job;
    worker->sinks = m_sinks;
    worker->onEvent = m_onEvent;

    const core::Status loaded = worker->runner.init(job.stages, "job " + job.id);
    if (!loaded.ok()) {
        // Kept, not discarded: the status endpoint has to be able to say WHY a
        // job is not running, and an operator fixing a model path needs that
        // message.
        worker->note(loaded.error().message);
        VS_WARN(kCategory) << "job " << job.id << ": " << loaded.error().message;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_workers[job.id] = worker;
        return;
    }

    const core::Status jpegReady = worker->jpeg.start();
    if (!jpegReady.ok()) {
        // Not fatal: detections are still worth publishing without a picture.
        VS_WARN(kCategory) << "job " << job.id << ": no JPEG encoder: "
                           << jpegReady.error().message;
    }

    std::shared_ptr<FrameTap> tap;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tap = tapFor(job.cameraId);
    }
    if (!tap) {
        worker->note("the camera is not streaming yet");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_workers[job.id] = worker;
        return;
    }

    worker->tap = tap;
    worker->running.store(true);
    worker->thread = std::thread([worker] { worker->loop(); });
    worker->tapSinkId = tap->addSink([worker](const core::ImageView& frame, std::int64_t tsUs) {
        worker->offer(frame, tsUs);
    });

    std::lock_guard<std::mutex> lock(m_mutex);
    m_workers[job.id] = worker;
}

void AiRuntime::restartJob(const std::string& jobId) {
    vision::AiJob job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_workers.find(jobId);
        if (it == m_workers.end()) return;
        job = it->second->job;
    }
    applyJob(job);
}

void AiRuntime::removeJob(const std::string& jobId) {
    std::shared_ptr<Worker> worker;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_workers.find(jobId);
        if (it == m_workers.end()) return;
        worker = it->second;
        m_workers.erase(it);
    }
    // Outside the lock: joining a worker waits for an inference to finish, and
    // holding the runtime's lock for that would stall every other job.
    worker->running.store(false);
    worker->wake.notify_all();
    if (worker->tap && worker->tapSinkId != 0) worker->tap->removeSink(worker->tapSinkId);
    if (worker->thread.joinable()) worker->thread.join();
    worker->jpeg.stop();
}

std::vector<AiJobStatus> AiRuntime::statuses() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<AiJobStatus> out;
    out.reserve(m_workers.size());
    for (const auto& [id, worker] : m_workers) {
        AiJobStatus status;
        status.jobId = id;
        status.cameraId = worker->job.cameraId;
        status.running = worker->running.load();
        status.lastError = worker->error();
        status.framesAnalysed = worker->framesAnalysed.load();
        status.resultsPublished = worker->resultsPublished.load();
        status.lastInferenceMs = worker->lastInferenceMs.load();
        out.push_back(std::move(status));
    }
    return out;
}

}  // namespace visora::media
