#include "media/source/PlaybackSource.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "core/Log.hpp"
#include "core/Time.hpp"
#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "playback";

// How long a pull waits before the feeder rechecks whether it should stop or a
// control changed.
constexpr GstClockTime kPullTimeout = 200 * GST_MSECOND;

// Never sleep longer than this in one go, so a seek or a pause is acted on
// promptly even when the next frame is far away at 0.25x.
constexpr auto kMaxSleep = std::chrono::milliseconds(100);

double clampRate(double rate) {
    // Below this a session looks hung; above it, even keyframe-only playback
    // outruns what a decoder will keep up with.
    return std::clamp(rate, 0.125, 32.0);
}

}  // namespace

struct PlaybackSource::Impl {
    GstElement* pipeline = nullptr;
    GstElement* appsink = nullptr;

    // The segment currently open.
    std::int64_t segmentStartMs = 0;
    std::int64_t segmentEndMs = 0;
    // Wall-clock instant of the first frame we are willing to emit. Frames
    // before it come from the run-back and are consumed without pacing.
    std::int64_t catchUpToMs = 0;
    // Where the feeder has got to, published for the status endpoint.
    std::atomic<std::int64_t> positionMs{0};

    // Controls, read by the feeder at each step.
    std::mutex controlMutex;
    std::condition_variable wake;
    double rate = 1.0;
    bool paused = false;
    std::int64_t seekTo = -1;   // -1 = no seek pending
    double activeRate = 1.0;

    std::atomic<bool> running{false};
    std::atomic<bool> ended{false};
    std::thread feeder;

    mutable std::mutex sinkMutex;
    std::map<std::uint64_t, Sink> sinks;
    std::uint64_t nextSinkId = 1;
};

PlaybackSource::PlaybackSource(std::string cameraId,
                               std::shared_ptr<RecordingRepository> recordings, Codec codec,
                               std::int64_t startMs)
    : m_cameraId(std::move(cameraId)),
      m_recordings(std::move(recordings)),
      m_codec(codec),
      m_impl(std::make_unique<Impl>()) {
    m_impl->positionMs.store(startMs);
    m_impl->seekTo = startMs;
}

PlaybackSource::~PlaybackSource() { stop(); }

core::Status PlaybackSource::start() {
    if (m_impl->running.exchange(true)) return {};
    if (m_codec == Codec::Unknown) {
        m_impl->running.store(false);
        return core::invalidArgument("playback needs to know the recording's codec");
    }
    m_impl->feeder = std::thread([this] { feederLoop(); });
    return {};
}

void PlaybackSource::stop() {
    if (!m_impl->running.exchange(false)) return;
    m_impl->wake.notify_all();
    if (m_impl->feeder.joinable()) m_impl->feeder.join();
    closeFile();
}

void PlaybackSource::seek(std::int64_t wallMs) {
    {
        std::lock_guard<std::mutex> lock(m_impl->controlMutex);
        m_impl->seekTo = wallMs;
        // A seek always resumes: an operator clicking the timeline while paused
        // means "show me there", not "stay frozen".
        m_impl->paused = false;
    }
    m_impl->ended.store(false);
    m_impl->wake.notify_all();
}

void PlaybackSource::setRate(double rate) {
    {
        std::lock_guard<std::mutex> lock(m_impl->controlMutex);
        m_impl->rate = clampRate(rate);
    }
    m_impl->wake.notify_all();
}

void PlaybackSource::setPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(m_impl->controlMutex);
        m_impl->paused = paused;
    }
    m_impl->wake.notify_all();
}

PlaybackState PlaybackSource::state() const {
    PlaybackState out;
    out.positionMs = m_impl->positionMs.load();
    out.ended = m_impl->ended.load();
    std::lock_guard<std::mutex> lock(m_impl->controlMutex);
    out.rate = m_impl->rate;
    out.paused = m_impl->paused;
    return out;
}

std::uint64_t PlaybackSource::addSink(Sink sink) {
    std::lock_guard<std::mutex> lock(m_impl->sinkMutex);
    const std::uint64_t id = m_impl->nextSinkId++;
    m_impl->sinks.emplace(id, std::move(sink));
    return id;
}

void PlaybackSource::removeSink(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(m_impl->sinkMutex);
    m_impl->sinks.erase(id);
}

bool PlaybackSource::alive() const {
    // `ended` is NOT death. Reaching the end of what was recorded is an
    // ordinary state of a timeline, and the client can seek somewhere else on
    // the connection it already has — which is the entire reason the session
    // stays open. Consulting `ended` here reaped a session the moment fast
    // playback ran past the end of the recording, seconds into a scrub.
    return m_impl->running.load();
}

void PlaybackSource::closeFile() {
    if (m_impl->pipeline) gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
    if (m_impl->appsink) {
        gst_object_unref(m_impl->appsink);
        m_impl->appsink = nullptr;
    }
    if (m_impl->pipeline) {
        gst_object_unref(m_impl->pipeline);
        m_impl->pipeline = nullptr;
    }
}

bool PlaybackSource::openAt(std::int64_t wallMs) {
    closeFile();

    // A day either side: wide enough that a seek near a gap finds the next
    // recording, bounded so the query does not walk a year of segments.
    constexpr std::int64_t kWindowMs = 24LL * 60 * 60 * 1000;
    auto segments = m_recordings->segmentsInRange(m_cameraId, wallMs - kWindowMs,
                                                  wallMs + kWindowMs);
    if (!segments) {
        VS_WARN(kCategory) << m_cameraId << ": " << segments.error().message;
        return false;
    }

    // Only finished segments: the one being written has no reliable end, and
    // reading it while the muxer writes gives a truncated file.
    std::vector<RecordingSegment> complete;
    for (const RecordingSegment& segment : segments.value()) {
        if (segment.status == SegmentStatus::Complete) complete.push_back(segment);
    }

    const SeekPoint point = seekTo(complete, wallMs);
    if (!point.found) return false;

    m_impl->segmentStartMs = point.segment.startMs;
    m_impl->segmentEndMs = point.segment.endMs;

    // Start decoding before the mark so a keyframe certainly precedes it. The
    // surplus is consumed at full speed — see catchUpToMs.
    const std::int64_t offsetMs = std::max<std::int64_t>(0, point.offsetMs - kSeekRunbackMs);
    m_impl->catchUpToMs = point.segment.startMs + point.offsetMs;

    LaunchPipeline pipeline;
    pipeline.chain()
        .add(ElementSpec("filesrc").setQuoted("location", point.segment.path))
        // tsdemux has sometimes-pads; gst_parse_launch defers the link itself,
        // so there is no pad-added signal to catch.
        .add(ElementSpec("tsdemux").named("d"))
        .add(ElementSpec(parser(m_codec)).set("config-interval", -1))
        .caps(parsedCaps(m_codec))
        // drop=false with a small max-buffers is the throttle: while the feeder
        // sleeps waiting for a frame's due time, the pipeline stalls behind the
        // full appsink. Without it the whole file is read into memory at once.
        .add(ElementSpec("appsink")
                 .named("out")
                 .set("sync", false)
                 .set("max-buffers", 8)
                 .set("drop", false));

    const std::string launch = pipeline.toLaunch(/*wrapped=*/false);
    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(launch.c_str(), &error);
    if (!m_impl->pipeline || error) {
        VS_WARN(kCategory) << m_cameraId << ": " << (error && error->message ? error->message
                                                                             : "bad pipeline");
        if (error) g_error_free(error);
        closeFile();
        return false;
    }

    m_impl->appsink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "out");
    if (!m_impl->appsink) {
        closeFile();
        return false;
    }

    // PAUSED and wait for preroll before seeking: a seek on a pipeline with no
    // state yet is silently ignored, and playback then starts from the
    // beginning of the file whatever was asked for.
    gst_element_set_state(m_impl->pipeline, GST_STATE_PAUSED);
    GstState state = GST_STATE_NULL;
    if (gst_element_get_state(m_impl->pipeline, &state, nullptr, 3 * GST_SECOND) ==
        GST_STATE_CHANGE_FAILURE) {
        closeFile();
        return false;
    }
    if (offsetMs > 0) {
        gst_element_seek_simple(m_impl->pipeline, GST_FORMAT_TIME,
                                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                          GST_SEEK_FLAG_KEY_UNIT |
                                                          GST_SEEK_FLAG_SNAP_BEFORE),
                                offsetMs * GST_MSECOND);
    }
    gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING);

    VS_DEBUG(kCategory) << m_cameraId << ": playing " << point.segment.path << " from +"
                        << offsetMs << "ms";
    return true;
}

void PlaybackSource::deliver(void* rawSample, std::int64_t wallMs, bool keyframe) {
    auto* sample = static_cast<GstSample*>(rawSample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    if (!buffer) return;

    m_impl->positionMs.store(wallMs);
    (void)keyframe;

    std::vector<Sink> targets;
    {
        std::lock_guard<std::mutex> lock(m_impl->sinkMutex);
        targets.reserve(m_impl->sinks.size());
        for (const auto& [id, sink] : m_impl->sinks) targets.push_back(sink);
    }
    for (const Sink& sink : targets) sink(buffer, caps);
}

void PlaybackSource::feederLoop() {
    VS_INFO(kCategory) << m_cameraId << ": playback session started";

    // When the current frame is due, in real time. Reset on every seek and rate
    // change, so the pacing does not try to make up time it deliberately spent.
    auto nextDue = std::chrono::steady_clock::now();
    std::int64_t lastFrameMs = -1;

    while (m_impl->running.load()) {
        std::int64_t seekTarget = -1;
        double rate = 1.0;
        bool paused = false;
        {
            std::unique_lock<std::mutex> lock(m_impl->controlMutex);
            seekTarget = m_impl->seekTo;
            m_impl->seekTo = -1;
            rate = m_impl->rate;
            paused = m_impl->paused;
            if (paused && seekTarget < 0) {
                m_impl->wake.wait_for(lock, kMaxSleep);
                continue;
            }
            m_impl->activeRate = rate;
        }

        if (seekTarget >= 0) {
            if (!openAt(seekTarget)) {
                // Nothing recorded there. Not an error — a timeline has gaps —
                // so report ended and wait for the next command.
                m_impl->ended.store(true);
                std::unique_lock<std::mutex> lock(m_impl->controlMutex);
                m_impl->wake.wait_for(lock, kMaxSleep);
                continue;
            }
            m_impl->ended.store(false);
            nextDue = std::chrono::steady_clock::now();
            lastFrameMs = -1;
        }

        if (!m_impl->appsink) {
            std::unique_lock<std::mutex> lock(m_impl->controlMutex);
            m_impl->wake.wait_for(lock, kMaxSleep);
            continue;
        }

        GstSample* sample =
            gst_app_sink_try_pull_sample(GST_APP_SINK(m_impl->appsink), kPullTimeout);
        if (!sample) {
            if (!gst_app_sink_is_eos(GST_APP_SINK(m_impl->appsink))) continue;
            // End of this file: continue with whatever was recorded next. A gap
            // is skipped rather than waited out, which is what an operator
            // scrubbing a night of motion-only recording wants.
            const std::int64_t next = m_impl->segmentEndMs + 1;
            if (!openAt(next)) {
                m_impl->ended.store(true);
                std::unique_lock<std::mutex> lock(m_impl->controlMutex);
                m_impl->wake.wait_for(lock, kMaxSleep);
                continue;
            }
            nextDue = std::chrono::steady_clock::now();
            lastFrameMs = -1;
            continue;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        const bool keyframe =
            buffer && !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        const std::int64_t inFileMs =
            buffer && GST_BUFFER_PTS_IS_VALID(buffer)
                ? static_cast<std::int64_t>(GST_BUFFER_PTS(buffer) / GST_MSECOND)
                : 0;
        const std::int64_t wallMs = m_impl->segmentStartMs + inFileMs;

        // Frames from the run-back: consume them as fast as they come, so the
        // decoder has its references without the click feeling slow.
        if (wallMs < m_impl->catchUpToMs) {
            gst_sample_unref(sample);
            continue;
        }

        // At speed, send keyframes only. A recording with one keyframe per
        // second then gives 8 pictures a second at 8x — the faster the scrub,
        // the smoother it looks, rather than the reverse.
        if (rate >= kKeyframeOnlyRate && !keyframe) {
            gst_sample_unref(sample);
            continue;
        }

        // Pace it. This is where "4x" becomes four seconds of content per real
        // second; nothing downstream knows about speed at all.
        if (lastFrameMs >= 0 && wallMs > lastFrameMs) {
            const double contentMs = static_cast<double>(wallMs - lastFrameMs);
            nextDue += std::chrono::microseconds(
                static_cast<std::int64_t>(contentMs * 1000.0 / std::max(0.001, rate)));
        }
        lastFrameMs = wallMs;

        while (m_impl->running.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextDue) break;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(nextDue - now);
            std::unique_lock<std::mutex> lock(m_impl->controlMutex);
            // Woken early by a control change, which is why this is a wait and
            // not a sleep.
            m_impl->wake.wait_for(lock, std::min<std::chrono::milliseconds>(remaining,
                                                                            kMaxSleep));
            if (m_impl->seekTo >= 0 || m_impl->rate != rate || m_impl->paused) break;
        }

        deliver(sample, wallMs, keyframe);
        gst_sample_unref(sample);
    }

    VS_INFO(kCategory) << m_cameraId << ": playback session stopped";
}

}  // namespace visora::media
