// The Unix-socket sink: the output gstreamer_ai_python consumes.
//
// One framed message per result, in the format ResultWire pins byte for byte.
// Several consumers may connect at once; each gets every message.
//
// This is the compatibility path. It exists to keep a separately deployed
// Python component working unchanged, so its behaviour is copied from the
// predecessor rather than improved: same framing, same socket semantics, same
// treatment of a consumer that stops reading.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.hpp"
#include "vision/ResultSink.hpp"
#include "vision/ResultWire.hpp"

namespace visora::vision {
namespace {

constexpr const char* kCategory = "result-sink";
constexpr const char* kDefaultPath = "/tmp/ai_engine.sock";

// A consumer that stops reading must not stall the pipeline.
//
// publish() holds the client lock while sending, so once the socket buffer
// fills, a stuck consumer would block every AI job worker indefinitely. After
// this long with no progress the consumer is treated as dead and dropped; it
// reconnects on its own.
constexpr int kSendTimeoutSeconds = 2;

std::string socketPath() {
    if (const char* env = std::getenv("VISORA_RESULT_SOCKET")) {
        if (env[0] != '\0') return env;
    }
    return kDefaultPath;
}

class UnixSocketSink final : public ResultSink {
public:
    ~UnixSocketSink() override { stop(); }

    std::string_view id() const override { return "unix-socket"; }

    core::Status start() override {
        m_path = socketPath();

        m_listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_listenFd < 0) {
            return core::internalError(std::string("socket(): ") + std::strerror(errno));
        }

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (m_path.size() >= sizeof(address.sun_path)) {
            closeListener();
            return core::invalidArgument("socket path too long: " + m_path);
        }
        std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);

        // A stale socket file from a previous run would make bind() fail.
        ::unlink(m_path.c_str());

        if (::bind(m_listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            const std::string why = std::strerror(errno);
            closeListener();
            return core::internalError("bind(" + m_path + "): " + why);
        }
        if (::listen(m_listenFd, 4) < 0) {
            const std::string why = std::strerror(errno);
            closeListener();
            return core::internalError("listen(" + m_path + "): " + why);
        }

        m_running.store(true);
        m_acceptThread = std::thread([this] { acceptLoop(); });
        VS_INFO(kCategory) << "unix-socket sink listening on " << m_path;
        return {};
    }

    void stop() override {
        if (!m_running.exchange(false)) return;

        // Shutting the listener down is what wakes the accept loop.
        if (m_listenFd >= 0) ::shutdown(m_listenFd, SHUT_RDWR);
        closeListener();
        if (m_acceptThread.joinable()) m_acceptThread.join();

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const int fd : m_clients) ::close(fd);
        m_clients.clear();
        ::unlink(m_path.c_str());
    }

    void publish(const Result& result) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_clients.empty()) return;

        // Encoded once for every consumer.
        const std::vector<std::uint8_t> message = wire::encode(result);

        for (auto it = m_clients.begin(); it != m_clients.end();) {
            if (sendAll(*it, message.data(), message.size())) {
                ++it;
                continue;
            }
            VS_WARN(kCategory) << "consumer on fd " << *it
                               << " stopped reading or disconnected; dropping it";
            ::close(*it);
            it = m_clients.erase(it);
        }
    }

    bool hasConsumers() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_clients.empty();
    }

private:
    void closeListener() {
        if (m_listenFd >= 0) {
            ::close(m_listenFd);
            m_listenFd = -1;
        }
    }

    void acceptLoop() {
        while (m_running.load()) {
            const int fd = ::accept(m_listenFd, nullptr, nullptr);
            if (fd < 0) {
                if (m_running.load()) {
                    VS_WARN(kCategory) << "accept(): " << std::strerror(errno);
                }
                break;
            }

            timeval timeout{};
            timeout.tv_sec = kSendTimeoutSeconds;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

            std::lock_guard<std::mutex> lock(m_mutex);
            m_clients.push_back(fd);
            VS_INFO(kCategory) << "consumer connected (fd " << fd << ")";
        }
    }

    static bool sendAll(int fd, const std::uint8_t* data, std::size_t length) {
        std::size_t sent = 0;
        while (sent < length) {
            // MSG_NOSIGNAL: a consumer that vanishes mid-write must not kill
            // this process with SIGPIPE.
            const ssize_t written =
                ::send(fd, data + sent, length - sent, MSG_NOSIGNAL);
            if (written <= 0) return false;
            sent += static_cast<std::size_t>(written);
        }
        return true;
    }

    std::string m_path;
    int m_listenFd = -1;
    std::atomic<bool> m_running{false};
    std::thread m_acceptThread;
    mutable std::mutex m_mutex;
    std::vector<int> m_clients;
};

core::Probe probeUnixSocket() {
    return core::Probe::yes("Unix socket at " + socketPath() +
                            " (gstreamer_ai_python compatibility)");
}

const core::Register<ResultSink> registration{{
    "unix-socket",
    100,
    &probeUnixSocket,
    [] { return std::unique_ptr<ResultSink>(new UnixSocketSink()); },
}};

}  // namespace
}  // namespace visora::vision
