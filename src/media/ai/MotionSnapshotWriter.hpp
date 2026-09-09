#pragma once

// Saves the frame a motion event started on.
//
// ONE THREAD, off the decoder's. JpegEncoder is explicitly not to be called
// from a streaming thread — encoding takes milliseconds and the decoder is
// feeding every other consumer of that camera — so the frame is COPIED where it
// is still valid and encoded here.
//
// Only on the rising edge of an event, never per frame. The predecessor
// originally ran a jpegenc branch at one frame a second for every camera with
// motion enabled: 5.6% of a core each, and almost every picture thrown away
// because events are occasional. A snapshot per event is a few tens of
// milliseconds per event.
//
// The path is the predecessor's, unchanged, because rows already in the
// database use it and the same directory is served for both:
//
//     <dir>/<camera id>/<YYYY-MM-DD>/<epoch ms>.jpg
//
// A camera that produces events faster than they can be encoded drops the
// older ones rather than queueing: the useful picture is the one from the event
// happening now, and an unbounded queue of 460 KB frames is how a busy night
// becomes an out-of-memory.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/Image.hpp"
#include "core/Result.hpp"

namespace visora::media {

class JpegEncoder;

class MotionSnapshotWriter {
public:
    // Called once a file exists, with the path to store on the event.
    using Sink = std::function<void(const std::string& eventId, const std::string& path)>;

    MotionSnapshotWriter(std::string directory, int quality, Sink sink);
    ~MotionSnapshotWriter();

    MotionSnapshotWriter(const MotionSnapshotWriter&) = delete;
    MotionSnapshotWriter& operator=(const MotionSnapshotWriter&) = delete;

    core::Status start();
    void stop();

    // Takes ownership of a copy the caller already made. Returns immediately.
    void capture(const std::string& cameraId, const std::string& eventId,
                 std::shared_ptr<const core::OwnedImage> frame);

private:
    struct Pending {
        std::string cameraId;
        std::string eventId;
        std::shared_ptr<const core::OwnedImage> frame;
    };

    void loop();
    // Writes the bytes and returns the path stored on the event, or empty when
    // the write failed — an event with no picture beats a row pointing at a
    // file that is not there.
    std::string writeFile(const std::string& cameraId,
                          const std::vector<std::uint8_t>& jpeg) const;

    std::string m_directory;
    int m_quality;
    Sink m_sink;

    std::unique_ptr<JpegEncoder> m_encoder;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Pending> m_queue;
};

// How many events may be waiting to be encoded. Small on purpose: see above.
inline constexpr std::size_t kMaxPendingSnapshots = 4;

}  // namespace visora::media
