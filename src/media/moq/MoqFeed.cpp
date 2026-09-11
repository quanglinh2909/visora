#include "media/moq/MoqFeed.hpp"

#include "media/source/BackPressure.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <gst/gst.h>

#include "core/Json.hpp"
#include "core/Log.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "moq";

// A 1080p keyframe can be several hundred kilobytes, and the system default of
// around 200 KB would send the first frame of every GOP down the partial-write
// path. Larger here so that is the exception it is meant to be.
constexpr int kSendBufferBytes = 2 << 20;

void writeBigEndian(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * (bytes - 1 - i))) & 0xFF));
    }
}

}  // namespace

std::vector<std::uint8_t> moqFrameHeader(std::uint64_t ptsUs, std::uint32_t length,
                                         bool keyframe) {
    std::vector<std::uint8_t> out;
    out.reserve(kMoqFrameHeaderBytes);
    out.push_back(keyframe ? kMoqFlagKeyframe : 0);
    writeBigEndian(out, ptsUs, 8);
    writeBigEndian(out, length, 4);
    return out;
}

std::string moqSessionHeader(const std::string& feedId, const std::string& cameraId,
                             const std::string& sessionId, const std::string& codec) {
    std::string json = "MOQF1 {\"feed\":";
    core::appendJsonString(json, feedId);
    json += ",\"camera\":";
    core::appendJsonString(json, cameraId);
    json += ",\"session\":";
    core::appendJsonString(json, sessionId);
    json += ",\"codec\":";
    core::appendJsonString(json, codec);
    json += "}\n";
    return json;
}

struct MoqFeed::Wire {
    std::mutex mutex;
    int fd = -1;
    std::atomic<bool> alive{false};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> dropped{0};
    // What is left of ONE frame that was written partially. Never more than
    // one: a new frame is only begun when this is empty.
    std::vector<std::uint8_t> pending;
    // After a drop, nothing is sent until a keyframe. The frames in between
    // reference pictures the reader never got, so they decode to tearing and
    // cost exactly the bandwidth that was already short. Both reference servers
    // drop a whole GOP for this reason.
    DropUntilKeyframe gate;

    // Pushes whatever is left of the partial frame. False when the connection
    // has died.
    bool flushPending();
    void push(GstBuffer* buffer);
    void close();
};

bool MoqFeed::Wire::flushPending() {
    while (!pending.empty()) {
        const ssize_t sent = ::send(fd, pending.data(), pending.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            pending.erase(pending.begin(), pending.begin() + sent);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;  // still full
        VS_WARN(kCategory) << "feed closed: " << std::strerror(errno);
        alive.store(false);
        return false;
    }
    return true;
}

void MoqFeed::Wire::push(GstBuffer* buffer) {
    if (!buffer) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (fd < 0 || !alive.load()) return;

    if (!flushPending()) return;

    // The tail of the previous frame has not gone yet, so this one cannot be
    // begun: a frame sent partially and abandoned loses the reader its frame
    // alignment for the rest of the session.
    const bool behind = !pending.empty();
    const bool keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    if (!gate.admit(keyframe, behind)) {
        dropped.fetch_add(1);
        return;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    const std::uint64_t ptsUs =
        GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) / 1000 : 0;
    const auto header =
        moqFrameHeader(ptsUs, static_cast<std::uint32_t>(map.size), keyframe);

    // One writev, so the header and payload cannot be separated by a partial
    // write in a way that needs two pieces of bookkeeping.
    iovec parts[2];
    parts[0].iov_base = const_cast<std::uint8_t*>(header.data());
    parts[0].iov_len = header.size();
    parts[1].iov_base = map.data;
    parts[1].iov_len = map.size;

    msghdr message{};
    message.msg_iov = parts;
    message.msg_iovlen = 2;

    const ssize_t sent = ::sendmsg(fd, &message, MSG_NOSIGNAL);
    const std::size_t total = header.size() + map.size;

    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Nothing went. Keep the whole frame as the pending tail.
            pending.assign(header.begin(), header.end());
            pending.insert(pending.end(), map.data, map.data + map.size);
        } else {
            VS_WARN(kCategory) << "feed closed: " << std::strerror(errno);
            alive.store(false);
        }
    } else if (static_cast<std::size_t>(sent) < total) {
        // Partial. Keep exactly the remainder.
        std::vector<std::uint8_t> whole;
        whole.reserve(total);
        whole.insert(whole.end(), header.begin(), header.end());
        whole.insert(whole.end(), map.data, map.data + map.size);
        pending.assign(whole.begin() + sent, whole.end());
        frames.fetch_add(1);
    } else {
        frames.fetch_add(1);
    }

    gst_buffer_unmap(buffer, &map);
}

void MoqFeed::Wire::close() {
    std::lock_guard<std::mutex> lock(mutex);
    alive.store(false);
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
    pending.clear();
}

MoqFeed::MoqFeed(std::string sessionId, std::string cameraId, std::string feedId,
                 std::string socketPath, std::shared_ptr<EncodedSource> source)
    : m_sessionId(std::move(sessionId)),
      m_cameraId(std::move(cameraId)),
      m_feedId(std::move(feedId)),
      m_socketPath(std::move(socketPath)),
      m_source(std::move(source)),
      m_wire(std::make_shared<Wire>()) {}

MoqFeed::~MoqFeed() { stop(); }

core::Status MoqFeed::start() {
    if (!m_source) return core::invalidArgument("no source for this feed");

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return core::internalError(std::string("socket: ") + std::strerror(errno));

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    // sun_path is 108 bytes and truncation would connect to the wrong place, so
    // this is refused rather than silently shortened.
    if (m_socketPath.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return core::invalidArgument("the MoQ socket path is too long: " + m_socketPath);
    }
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", m_socketPath.c_str());

    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string reason = std::strerror(errno);
        ::close(fd);
        return core::unsupported("the MoQ server is not listening on " + m_socketPath + ": " +
                                 reason);
    }

    // The header goes out BLOCKING: it is a few dozen bytes, and a session
    // whose header did not arrive is meaningless — better to fail here than
    // quietly later.
    const std::string header =
        moqSessionHeader(m_feedId, m_cameraId, m_sessionId, toString(m_source->codec()));
    std::size_t at = 0;
    while (at < header.size()) {
        const ssize_t sent =
            ::send(fd, header.data() + at, header.size() - at, MSG_NOSIGNAL);
        if (sent <= 0) {
            const std::string reason = std::strerror(errno);
            ::close(fd);
            return core::internalError("could not send the MoQ header: " + reason);
        }
        at += static_cast<std::size_t>(sent);
    }

    int sendBuffer = kSendBufferBytes;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
    // Non-blocking from here on. The sink runs on the source's streaming thread,
    // and blocking there would stall recording and every other viewer of the
    // SAME camera — one slow reader must not take the rest down with it.
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    {
        std::lock_guard<std::mutex> lock(m_wire->mutex);
        m_wire->fd = fd;
        m_wire->alive.store(true);
    }

    m_sinkId = m_source->addSink(
        [wire = m_wire](GstBuffer* buffer, GstCaps*) { wire->push(buffer); });

    VS_INFO(kCategory) << m_sessionId << ": feeding " << m_feedId << " from " << m_cameraId
                       << " (" << toString(m_source->codec()) << ')';
    return {};
}

void MoqFeed::stop() {
    if (m_source && m_sinkId != 0) {
        m_source->removeSink(m_sinkId);
        m_sinkId = 0;
    }
    if (m_wire) m_wire->close();
}

bool MoqFeed::alive() const {
    if (!m_wire->alive.load()) return false;
    return m_source && m_source->alive();
}

std::uint64_t MoqFeed::framesSent() const { return m_wire->frames.load(); }
std::uint64_t MoqFeed::framesDropped() const { return m_wire->dropped.load(); }

}  // namespace visora::media
