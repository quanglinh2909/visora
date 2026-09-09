#pragma once

// Feeds one viewer's frames to the MoQ server over a Unix socket.
//
// THE WIRE FORMAT IS A PUBLISHED CONTRACT. A separately deployed QUIC server
// reads it, so it may not change without coordinating a deployment — the same
// standing as the AI result socket. All numbers are big-endian:
//
//     header      "MOQF1 " + one line of JSON + '\n'
//     each frame  u8 flags | u64 pts_us | u32 length | Annex-B bytes
//     flags bit 0 the frame is a keyframe
//
// WHY THIS CLASS IS SO SMALL: everything expensive — pulling the camera,
// decoding, transcoding — is done by the shared source and is ALREADY being
// done for recording and for the WebRTC viewers. Adding a MoQ viewer is adding
// a sink to a source that exists, exactly like adding a WebRTC viewer. There is
// no pipeline here.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "media/source/EncodedSource.hpp"

namespace visora::media {

inline constexpr std::size_t kMoqFrameHeaderBytes = 13;
inline constexpr std::uint8_t kMoqFlagKeyframe = 0x01;

// The bytes of one frame's header, exposed so the format can be pinned by a
// test rather than trusted.
std::vector<std::uint8_t> moqFrameHeader(std::uint64_t ptsUs, std::uint32_t length,
                                         bool keyframe);

// The session header line.
std::string moqSessionHeader(const std::string& feedId, const std::string& cameraId,
                             const std::string& sessionId, const std::string& codec);

class MoqFeed {
public:
    MoqFeed(std::string sessionId, std::string cameraId, std::string feedId,
            std::string socketPath, std::shared_ptr<EncodedSource> source);
    ~MoqFeed();

    MoqFeed(const MoqFeed&) = delete;
    MoqFeed& operator=(const MoqFeed&) = delete;

    core::Status start();
    void stop();

    bool alive() const;

    const std::string& id() const { return m_sessionId; }
    const std::string& cameraId() const { return m_cameraId; }
    const std::string& feedId() const { return m_feedId; }
    std::uint64_t framesSent() const;
    std::uint64_t framesDropped() const;

private:
    struct Wire;

    std::string m_sessionId;
    std::string m_cameraId;
    std::string m_feedId;
    std::string m_socketPath;
    std::shared_ptr<EncodedSource> m_source;
    std::uint64_t m_sinkId = 0;
    std::shared_ptr<Wire> m_wire;
};

}  // namespace visora::media
