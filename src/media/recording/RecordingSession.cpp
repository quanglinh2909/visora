#include "media/recording/RecordingSession.hpp"

#include "media/source/BackPressure.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "core/Log.hpp"
#include "core/Time.hpp"
#include "media/gst/BusWatcher.hpp"
#include "media/gst/LaunchPipeline.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "recording";
constexpr const char* kAppSrcName = "rec_src";
constexpr const char* kSinkName = "rec_sink";

}  // namespace

struct RecordingSession::Impl {
    RecordingSession* owner = nullptr;
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* sink = nullptr;
    BusWatcher bus;
    std::uint64_t sinkId = 0;
    bool running = false;
    // Guards `enabled`, `capsSet` and the open-segment map, all of which the
    // source's streaming thread and the caller's thread both touch.
    std::mutex mutex;
    bool enabled = false;
    bool capsSet = false;
    // Guarded by `mutex`, like everything else on the push path.
    DropUntilKeyframe gate;
    std::string cameraId;

    // Set by the bus watcher when the pipeline finishes draining. A promise
    // rather than a second reader of the bus: stop() used to pop the bus itself
    // while the watcher thread was popping it too, so whichever got there first
    // consumed the EOS and the other waited out its whole timeout.
    std::mutex drainMutex;
    std::condition_variable drained;
    bool drainedFlag = false;

    // Segments the muxer has opened and not yet closed, keyed by file path —
    // which is the only thing the "opened" and "closed" messages share.
    struct Open {
        std::int64_t startedMs = 0;      // wall clock, for the index
        std::uint64_t runningTimeNs = 0; // media clock, for the duration
    };
    std::map<std::string, Open> open;

    void push(GstBuffer* buffer, GstCaps* caps);
};

void RecordingSession::Impl::push(GstBuffer* buffer, GstCaps* caps) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!enabled || !appsrc) return;

    if (!capsSet && caps) {
        gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
        capsSet = true;
    }

    // A disk that cannot keep up must cost bounded memory rather than the
    // process. Given a lot of room first — see kMaxRecordingQueuedBytes — since
    // a gap in a recording is worse than a gap in a live view.
    const std::uint64_t queued = gst_app_src_get_current_level_bytes(GST_APP_SRC(appsrc));
    const bool keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    if (!gate.admit(keyframe, queued > kMaxRecordingQueuedBytes)) {
        if (gate.dropped() % 100 == 1) {
            VS_WARN(kCategory) << cameraId << ": recording cannot keep up with the disk, "
                               << queued / (1024 * 1024) << " MiB queued; dropping until the "
                               << "next keyframe (" << gate.dropped() << " so far)";
        }
        return;
    }

    // A shallow copy, because the buffer is shared with every other consumer of
    // this source and appsrc is about to write a timestamp onto it. The copy
    // shares the payload, so this is cheap.
    GstBuffer* out = gst_buffer_make_writable(gst_buffer_ref(buffer));

    // Clear the source's timestamps so do-timestamp=true applies this
    // pipeline's clock.
    //
    // The buffer carries PTS from the SOURCE pipeline's clock, which is a
    // different clock with a different base. splitmuxsink decides where to cut
    // a segment from the timestamps it sees; fed another pipeline's, it cuts in
    // the wrong places or not at all. do-timestamp only stamps a buffer that
    // has none, so they have to be cleared here rather than merely ignored.
    GST_BUFFER_PTS(out) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(out) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(out) = GST_CLOCK_TIME_NONE;

    gst_app_src_push_buffer(GST_APP_SRC(appsrc), out);
}

namespace {

gchar* onFormatLocation(GstElement*, guint, gpointer user) {
    auto* session = static_cast<RecordingSession*>(user);
    // Named for the operator, not for the machine: someone browsing the
    // recordings directory with `ls` reads these, and a name in UTC sends them
    // to the wrong hour. The database holds the authoritative instant.
    return g_strdup(session->nextSegmentPath().c_str());
}

void handleBusMessage(RecordingSession* session, RecordingSession::Impl* impl,
                      GstMessage* message);

}  // namespace

RecordingSession::RecordingSession(std::string cameraId, std::shared_ptr<EncodedSource> source,
                                   RecordingOptions options, SegmentSink onSegment)
    : m_cameraId(std::move(cameraId)),
      m_source(std::move(source)),
      m_options(std::move(options)),
      m_onSegment(std::move(onSegment)),
      m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
}

RecordingSession::~RecordingSession() { stop(); }

std::string RecordingSession::nextSegmentPath() const {
    return segmentPath(m_options.recordingDir, m_cameraId, core::nowLocalFileTimestamp());
}

std::string RecordingSession::launchFor(const std::string& cameraId, Codec codec,
                                        const RecordingOptions& options) {
    if (codec == Codec::Unknown) return {};

    const std::uint64_t segmentNs =
        static_cast<std::uint64_t>(std::max(1, options.segmentSeconds)) * 1'000'000'000ULL;

    LaunchPipeline pipeline;
    pipeline.chain()
        // The source is the shared camera source pushing parsed access units,
        // not an rtspsrc of our own. do-timestamp=true because those buffers
        // arrive stripped of the source pipeline's clock — see Impl::push.
        .add(ElementSpec("appsrc")
                 .named(kAppSrcName)
                 .set("is-live", true)
                 .set("format", "time")
                 .set("do-timestamp", true)
                 .set("max-bytes", 0)
                 .set("block", false))
        .caps(parsedCaps(codec))
        .add("queue")
        .add(ElementSpec("splitmuxsink")
                 .named(kSinkName)
                 // Finalise a closed segment on another thread, so cutting a
                 // segment does not stall the incoming stream.
                 .set("async-finalize", true)
                 // Without this, the fragment-opened / fragment-closed messages
                 // never leave the sink's internal bin and nothing knows a file
                 // was written.
                 .set("message-forward", true)
                 // MPEG-TS, not MP4: a TS file that was cut off mid-write is
                 // still playable, and a recorder is killed by power loss more
                 // often than by anything else. An MP4 without its moov atom is
                 // a lost segment.
                 .set("muxer-factory", "mpegtsmux")
                 .set("max-size-time", static_cast<long long>(segmentNs))
                 // Ask the encoder for a keyframe at each boundary so every
                 // segment starts with one and can be played on its own.
                 .set("send-keyframe-requests", true));

    // NOT wrapped: gst_parse_launch wants an unparenthesised description.
    (void)cameraId;
    return pipeline.toLaunch(/*wrapped=*/false);
}

core::Status RecordingSession::start() {
    stop();

    if (!m_source) return core::invalidArgument("recording needs a source");
    if (m_options.mode == RecordingMode::Off) return {};

    const std::string launch = launchFor(m_cameraId, m_source->codec(), m_options);
    if (launch.empty()) {
        return core::unsupported("cannot record codec " +
                                 std::string(toString(m_source->codec())));
    }

    // The muxer will not create it, and a missing directory fails every
    // segment silently rather than once loudly.
    std::error_code ec;
    std::filesystem::create_directories(m_options.recordingDir + "/" + m_cameraId, ec);
    if (ec) {
        return core::internalError("cannot create the recording directory: " + ec.message());
    }

    VS_DEBUG(kCategory) << m_cameraId << ": " << launch;
    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(launch.c_str(), &error);
    if (!m_impl->pipeline || error) {
        const std::string message =
            error && error->message ? error->message : "could not build the recording pipeline";
        if (error) g_error_free(error);
        stop();
        return core::internalError(message);
    }

    m_impl->appsrc = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), kAppSrcName);
    m_impl->sink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), kSinkName);
    if (!m_impl->appsrc || !m_impl->sink) {
        stop();
        return core::internalError("recording pipeline is missing elements");
    }

    g_signal_connect(m_impl->sink, "format-location", G_CALLBACK(&onFormatLocation), this);

    m_impl->drainedFlag = false;
    m_impl->bus.start(m_impl->pipeline, m_cameraId, [this](GstMessage* message) {
        handleBusMessage(this, m_impl.get(), message);
    });

    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        stop();
        return core::hardwareFailure("could not start recording " + m_cameraId);
    }

    m_sessionStartMs = core::nowEpochMs();
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->enabled = true;
    }
    // Attach LAST, once the pipeline is playing: buffers arriving at an appsrc
    // in a non-playing pipeline queue up and the first segment starts late.
    m_impl->sinkId = m_source->addSink(
        [impl = m_impl.get()](GstBuffer* buffer, GstCaps* caps) { impl->push(buffer, caps); });
    m_impl->cameraId = m_cameraId;
    m_impl->running = true;

    VS_INFO(kCategory) << m_cameraId << ": recording to " << m_options.recordingDir << " ("
                       << m_options.segmentSeconds << "s segments, "
                       << toString(m_options.mode) << ')';
    return {};
}

void RecordingSession::stop() {
    // Order matters. Stop accepting buffers, then detach from the source, then
    // tear the pipeline down — detaching first would leave a push in flight
    // against an appsrc that is being destroyed.
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->enabled = false;
        m_impl->capsSet = false;
    }
    if (m_source && m_impl->sinkId != 0) {
        m_source->removeSink(m_impl->sinkId);
        m_impl->sinkId = 0;
    }

    if (m_impl->pipeline && m_impl->running) {
        // EOS first, so splitmuxsink finalises the segment it is writing and
        // reports it closed. Without this the last file is left marked
        // "recording" for ever: its footage is on disk, complete and valid, and
        // playback skips it because nothing ever said it finished.
        gst_element_send_event(m_impl->pipeline, gst_event_new_eos());

        std::unique_lock<std::mutex> lock(m_impl->drainMutex);
        m_impl->drained.wait_for(lock, std::chrono::seconds(3),
                                 [this] { return m_impl->drainedFlag; });
    }

    // Stopped AFTER the EOS wait above, which needs the messages still flowing,
    // and BEFORE the pipeline is unreffed, which the watcher points at.
    m_impl->bus.stop();
    if (m_impl->pipeline) gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
    if (m_impl->appsrc) {
        gst_object_unref(m_impl->appsrc);
        m_impl->appsrc = nullptr;
    }
    if (m_impl->sink) {
        gst_object_unref(m_impl->sink);
        m_impl->sink = nullptr;
    }
    if (m_impl->pipeline) {
        gst_object_unref(m_impl->pipeline);
        m_impl->pipeline = nullptr;
    }
    if (m_impl->running) {
        VS_INFO(kCategory) << m_cameraId << ": recording stopped";
        m_impl->running = false;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->open.clear();
}

bool RecordingSession::running() const { return m_impl->running; }

void RecordingSession::onFragmentOpened(const std::string& path,
                                        std::uint64_t runningTimeNs) {
    // The wall-clock instant is derived from the media clock, not read from the
    // system clock here: the message can be delivered late, and a start time
    // taken at delivery puts the segment where it was noticed rather than where
    // it is.
    const std::int64_t startedMs =
        m_sessionStartMs + static_cast<std::int64_t>(runningTimeNs / 1'000'000);
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->open[path] = Impl::Open{startedMs, runningTimeNs};
    }
    if (!m_onSegment) return;

    // Announced while still open, so a timeline has a live edge instead of
    // trailing one segment behind. The estimated end is the configured segment
    // length; the closing message replaces it with what was actually written.
    RecordingSegment segment;
    segment.cameraId = m_cameraId;
    segment.path = path;
    segment.startMs = startedMs;
    segment.durationMs = std::max(1, m_options.segmentSeconds) * 1000;
    segment.endMs = startedMs + segment.durationMs;
    segment.codec = m_source ? m_source->codec() : Codec::Unknown;
    segment.recordingMode = m_options.mode;
    segment.status = SegmentStatus::Recording;
    segment.sessionStartMs = m_sessionStartMs;
    m_onSegment(segment);
}

void RecordingSession::onFragmentClosed(const std::string& path,
                                        std::uint64_t runningTimeNs) {
    Impl::Open opened;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto it = m_impl->open.find(path);
        if (it == m_impl->open.end()) return;  // never saw it open; nothing to close
        opened = it->second;
        m_impl->open.erase(it);
    }
    if (!m_onSegment) return;

    // Media time, both ends. This is what makes the durations in a playlist
    // exact; wall-clock deltas around an asynchronous finalise were out by
    // whole seconds either way.
    const std::int64_t durationMs = static_cast<std::int64_t>(
        (runningTimeNs > opened.runningTimeNs ? runningTimeNs - opened.runningTimeNs : 0) /
        1'000'000);

    RecordingSegment segment;
    segment.cameraId = m_cameraId;
    segment.path = path;
    segment.startMs = opened.startedMs;
    segment.endMs = opened.startedMs + durationMs;
    segment.durationMs = static_cast<int>(durationMs);
    segment.codec = m_source ? m_source->codec() : Codec::Unknown;
    segment.recordingMode = m_options.mode;
    segment.status = SegmentStatus::Complete;
    segment.sessionStartMs = m_sessionStartMs;
    m_onSegment(segment);
}

namespace {

void handleBusMessage(RecordingSession* session, RecordingSession::Impl* impl,
                      GstMessage* message) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        VS_WARN(kCategory) << "recording pipeline error: " << errorTextOf(message);
    }
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS ||
        GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        // The fragment-closed message for the final file arrives BEFORE this,
        // so by now it has been recorded.
        std::lock_guard<std::mutex> lock(impl->drainMutex);
        impl->drainedFlag = true;
        impl->drained.notify_all();
        return;
    }
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ELEMENT) return;

    const GstStructure* structure = gst_message_get_structure(message);
    if (!structure) return;
    const gchar* name = gst_structure_get_name(structure);
    if (!name) return;
    const gchar* location = gst_structure_get_string(structure, "location");
    if (!location) return;

    guint64 runningTimeNs = 0;
    gst_structure_get_uint64(structure, "running-time", &runningTimeNs);

    if (g_strcmp0(name, "splitmuxsink-fragment-opened") == 0) {
        session->onFragmentOpened(location, runningTimeNs);
    } else if (g_strcmp0(name, "splitmuxsink-fragment-closed") == 0) {
        session->onFragmentClosed(location, runningTimeNs);
    }
}

}  // namespace

}  // namespace visora::media
