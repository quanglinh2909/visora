#pragma once

// One shared source per camera, handed to whoever asks for it.
//
// Recording, WebRTC and the AI pipeline all want the same camera's access
// units. Each opening its own RTSP connection would repeat the jitterbuffer,
// depayloader and parser for one stream, and would use up the handful of
// simultaneous sessions a camera permits — Dahua and Hikvision units cap this,
// so it is a constraint rather than an optimisation.
//
// Held by the registry, not by the last consumer.
//
// It used to be weak: the last consumer letting go destroyed the source and
// closed the connection to the camera, and nothing had to remember to release
// it. That is right for a board running seventeen cameras and wrong for the
// thing people do most — reloading the page cost a full RTSP re-handshake for a
// viewer who never really left.
//
// So a source outlives its last consumer by a grace period and a sweeper
// retires it after that, which is what `streamNoneReaderDelayMS` does in
// ZLMediaKit and what the publish timeouts do in SRS. Whether anything is
// consuming is asked of the SOURCE — its sink count — rather than tracked here,
// because that is the fact itself rather than a copy of it.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/Result.hpp"
#include "media/source/IdleRetirement.hpp"
#include "media/source/RtspEncodedSource.hpp"
#include "media/source/TranscodedSource.hpp"

namespace visora::media {

class CameraSourceRegistry {
public:
    explicit CameraSourceRegistry(RtspSourceOptions options = {});
    ~CameraSourceRegistry();

    CameraSourceRegistry(const CameraSourceRegistry&) = delete;
    CameraSourceRegistry& operator=(const CameraSourceRegistry&) = delete;

    // The camera's source, started, creating it if nobody holds one. A source
    // whose URL or codec no longer matches is replaced rather than reused: it
    // is pointed at a stream that is not the one being asked for.
    core::Result<std::shared_ptr<EncodedSource>> acquire(const std::string& cameraId,
                                                        const std::string& rtspUrl, Codec codec);

    // The camera's stream AS H.264, transcoding when it is not already.
    //
    // Shared per camera for the same reason the raw source is: decoding and
    // re-encoding 1080p is the most expensive thing this program does, and
    // doing it once per viewer is what makes a board fall over at four of them.
    // An H.264 camera is handed back unchanged — building a decode/encode pass
    // to convert H.264 to H.264 would be the most expensive no-op there is.
    core::Result<std::shared_ptr<EncodedSource>> acquireH264(const std::string& cameraId,
                                                            const std::string& rtspUrl,
                                                            Codec codec);

    // How many cameras currently have a live source. For diagnostics — a number
    // that does not match the number of streaming cameras means something is
    // holding a source it should have let go of.
    std::size_t liveCount() const;

    // Sources held only by the grace period — running, with nobody watching.
    // For diagnostics: a number that stays high means the grace period is too
    // long for how this deployment is used.
    std::size_t lingeringCount() const;

    // Runs one sweep now instead of waiting for the timer. Only for tests: it
    // makes the grace period assertable without sleeping through it.
    void sweepNow();

private:
    struct Entry {
        std::shared_ptr<RtspEncodedSource> source;
        std::string rtspUrl;
        Codec codec = Codec::Unknown;
        IdleTimer idle{std::chrono::milliseconds::zero()};
    };

    struct Transcode {
        std::shared_ptr<TranscodedSource> source;
        IdleTimer idle{std::chrono::milliseconds::zero()};
    };

    void sweep();
    void runSweeper();

    RtspSourceOptions m_options;
    mutable std::mutex m_mutex;
    std::map<std::string, Entry> m_entries;
    // Held the same way as the raw sources, and swept first: a transcode counts
    // as a consumer of its upstream, so the upstream cannot go idle until the
    // transcode has gone.
    std::map<std::string, Transcode> m_transcodes;

    std::condition_variable m_wake;
    std::atomic<bool> m_stopping{false};
    std::thread m_sweeper;

    // The last bitrate measured for each camera, remembered ACROSS source
    // lifetimes.
    //
    // Held behind a shared_ptr with its own lock because a source reports into
    // it from its streaming thread, and a source can outlive this registry
    // during shutdown.
    //
    // A transcode is built the moment the first viewer asks for one, which is
    // the same moment the camera source is created — so there is nothing to
    // measure yet and the encoder is left to guess. Its guess is width x height
    // x fps / 8, about 6.5 Mbps for 1080p, against cameras on this deployment
    // that send 790 kbps. Observed alternating in one log: the same camera
    // transcoded at "786 kbps" when a recorder had the source already running
    // and at "encoder default" when it had not.
    //
    // A camera's bitrate is a property of the CAMERA, not of one connection to
    // it, so remembering it is what makes the first viewer's stream as cheap as
    // the second's.
    struct BitrateMemory {
        std::mutex mutex;
        std::map<std::string, std::uint64_t> byCamera;

        void remember(const std::string& cameraId, std::uint64_t bps) {
            std::lock_guard<std::mutex> lock(mutex);
            byCamera[cameraId] = bps;
        }
        std::uint64_t recall(const std::string& cameraId) const {
            std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex));
            const auto it = byCamera.find(cameraId);
            return it == byCamera.end() ? 0 : it->second;
        }
    };
    std::shared_ptr<BitrateMemory> m_bitrates = std::make_shared<BitrateMemory>();
};

}  // namespace visora::media
