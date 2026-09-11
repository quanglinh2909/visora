#pragma once

// Recorded video as a source, with seek, pause and speed.
//
// WHY THIS EXISTS ALONGSIDE HLS: with HLS, every click on a timeline refetches
// a playlist describing the whole day — the predecessor measured 1.17 MB and
// 9,122 lines — and rebuilds the player. That cost grows with the length of the
// day rather than with what the operator wanted to look at. Here the session is
// opened ONCE and every later click is a seek command: jump to the file holding
// that instant, seek to the keyframe before it, carry on. No manifest, no
// player rebuild, and the cost does not change with how much has been recorded.
//
// THE PACING IS HERE, NOT IN THE PLAYER. A feeder thread reads ahead and sleeps
// until each frame is due, so "4x" means four seconds of content sent per real
// second. That is what makes something possible that HLS cannot do: from 4x
// upward only keyframes are sent, so bandwidth and decoding fall by an order of
// magnitude while the picture keeps stepping evenly. A recording with one
// keyframe per second gives 8 pictures a second at 8x and 16 at 16x — the
// faster you scrub, the smoother it gets, which is the opposite of how a
// browser fast-forwards.
//
// One source per session, unlike the live camera source which is shared: two
// people looking at two different moments have nothing to share.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/Result.hpp"
#include "media/recording/RecordingRepository.hpp"
#include "media/source/EncodedSource.hpp"

namespace visora::media {

// At or above this speed, only keyframes are sent. See the note above.
inline constexpr double kKeyframeOnlyRate = 4.0;

// How far before the requested instant to start decoding.
//
// Cameras here use a GOP of about two seconds; four is enough that a keyframe
// certainly precedes the mark however tsdemux's KEY_UNIT seek happens to land.
// The surplus is sent at full speed rather than paced, so it does not make the
// click feel slow.
inline constexpr std::int64_t kSeekRunbackMs = 4000;

struct PlaybackState {
    std::int64_t positionMs = 0;
    double rate = 1.0;
    bool paused = false;
    bool ended = false;
};

class PlaybackSource final : public EncodedSource {
public:
    PlaybackSource(std::string cameraId, std::shared_ptr<RecordingRepository> recordings,
                   Codec codec, std::int64_t startMs);
    ~PlaybackSource() override;

    PlaybackSource(const PlaybackSource&) = delete;
    PlaybackSource& operator=(const PlaybackSource&) = delete;

    core::Status start();
    void stop();

    // Controls. All safe from any thread; each takes effect at the feeder's
    // next step rather than immediately, which is what keeps the feeder free of
    // locks around GStreamer.
    void seek(std::int64_t wallMs);
    void setRate(double rate);
    void setPaused(bool paused);

    PlaybackState state() const;

    // --- EncodedSource -------------------------------------------------------
    std::uint64_t addSink(Sink sink, SinkOptions options = {}) override;
    void removeSink(std::uint64_t id) override;
    bool alive() const override;
    Codec codec() const override { return m_codec; }

private:
    struct Impl;

    void feederLoop();
    // Opens the segment covering `wallMs`. False when nothing is recorded there.
    bool openAt(std::int64_t wallMs);
    void closeFile();
    void deliver(void* sample, std::int64_t wallMs, bool keyframe);

    std::string m_cameraId;
    std::shared_ptr<RecordingRepository> m_recordings;
    Codec m_codec;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::media
