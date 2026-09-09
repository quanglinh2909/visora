#pragma once

// Writes one camera's stream to disk as a series of segments.
//
// Fed from the SHARED camera source rather than opening its own RTSP
// connection, so recording a camera someone is also watching does not pull it
// twice — and does not consume one of the handful of simultaneous streams a
// camera permits.
//
// Segments rather than one long file because everything a VMS does with
// recordings is bounded by time: play back a window, keep the minutes around an
// event, delete last month. A single file makes all three expensive and the
// last one impossible without rewriting it.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/Result.hpp"
#include "media/recording/Recording.hpp"
#include "media/source/EncodedSource.hpp"

namespace visora::media {

struct RecordingOptions {
    std::string recordingDir = "recordings";
    int segmentSeconds = 10;
    RecordingMode mode = RecordingMode::Continuous;
};

// Called as segments open and close. The session does not know what happens to
// them — the database, a retention policy, a websocket — which is what keeps
// GStreamer out of the layer that does.
//
// Called on a GStreamer thread. Do little; never throw.
using SegmentSink = std::function<void(const RecordingSegment&)>;

class RecordingSession {
public:
    RecordingSession(std::string cameraId, std::shared_ptr<EncodedSource> source,
                     RecordingOptions options, SegmentSink onSegment);
    ~RecordingSession();

    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    core::Status start();
    void stop();

    bool running() const;

    // When this session started, epoch ms. Stamped onto every segment it
    // produces: a new session means a new muxer whose clock restarts, and
    // playback has to know where those joins are. See HlsPlaylist.
    std::int64_t sessionStartMs() const { return m_sessionStartMs; }

    // The launch description, separated so it is asserted without a camera.
    static std::string launchFor(const std::string& cameraId, Codec codec,
                                 const RecordingOptions& options);

    // Where the next segment goes. Called by GStreamer through
    // format-location, and public only for that; not part of the API.
    std::string nextSegmentPath() const;

    // Called from the bus watch as splitmuxsink opens and closes files. Public
    // for the same reason.
    //
    // `runningTimeNs` is the muxer's own media clock, and it is what the
    // durations are computed from — never the wall clock. With
    // async-finalize=true the "closed" message arrives after the file is
    // actually finished, so wall-clock timing reports segments as seconds too
    // long or too short, and a player fed those drifts out of sync.
    void onFragmentOpened(const std::string& path, std::uint64_t runningTimeNs);
    void onFragmentClosed(const std::string& path, std::uint64_t runningTimeNs);

    // The GStreamer state. Public only because the bus callback is a free
    // function that receives it; nothing outside this class's own translation
    // unit names it.
    struct Impl;

private:

    std::string m_cameraId;
    std::shared_ptr<EncodedSource> m_source;
    RecordingOptions m_options;
    SegmentSink m_onSegment;
    std::int64_t m_sessionStartMs = 0;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::media
