#pragma once

// One shared source per camera, handed to whoever asks for it.
//
// Recording, WebRTC and the AI pipeline all want the same camera's access
// units. Each opening its own RTSP connection would repeat the jitterbuffer,
// depayloader and parser for one stream, and would use up the handful of
// simultaneous sessions a camera permits — Dahua and Hikvision units cap this,
// so it is a constraint rather than an optimisation.
//
// Held weakly: the last consumer letting go destroys the source, which closes
// the connection to the camera. Nothing has to remember to release it.

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/Result.hpp"
#include "media/source/RtspEncodedSource.hpp"
#include "media/source/TranscodedSource.hpp"

namespace visora::media {

class CameraSourceRegistry {
public:
    explicit CameraSourceRegistry(RtspSourceOptions options = {});

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

private:
    struct Entry {
        std::weak_ptr<RtspEncodedSource> source;
        std::string rtspUrl;
        Codec codec = Codec::Unknown;
    };

    RtspSourceOptions m_options;
    mutable std::mutex m_mutex;
    std::map<std::string, Entry> m_entries;
    // Held weakly like the raw sources: the last consumer letting go tears the
    // transcode down, and nothing has to remember to.
    std::map<std::string, std::weak_ptr<TranscodedSource>> m_transcodes;
};

}  // namespace visora::media
