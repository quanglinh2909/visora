// End-to-end check of the socket output, decoded exactly the way
// gstreamer_ai_python decodes it:
//
//   header = recv_exact(4); total_len = unpack(">I")
//   body   = recv_exact(total_len)
//   json_len = unpack(">I", body[:4]); meta = json(body[4:4+json_len])
//   full_jpeg = body[4+json_len:]
//
// Encoding the right bytes is only half the contract; this covers the other
// half — that they actually arrive that way over a real socket.

#include "TestHarness.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/Registry.hpp"
#include "vision/ResultSink.hpp"
#include "vision/ResultWire.hpp"

using namespace visora;
using visora::vision::Detection;
using visora::vision::Result;

namespace {

std::string testSocketPath() {
    return "/tmp/visora-sink-test-" + std::to_string(::getpid()) + ".sock";
}

bool recvExact(int fd, std::uint8_t* out, std::size_t want) {
    std::size_t got = 0;
    while (got < want) {
        const ssize_t n = ::recv(fd, out + got, want - got, 0);
        if (n <= 0) return false;
        got += static_cast<std::size_t>(n);
    }
    return true;
}

std::uint32_t readU32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

Result fixture() {
    Result r;
    r.cameraId = "cam-1";
    r.jobId = "job-1";
    r.seq = 7;
    r.tsUs = 1234567890;
    r.origWidth = 1920;
    r.origHeight = 1080;
    r.fullJpeg = {0xFF, 0xD8, 0x01, 0x02, 0x03, 0xFF, 0xD9};

    Detection d;
    d.x1 = 1;
    d.y1 = 2;
    d.x2 = 3;
    d.y2 = 4;
    d.score = 0.75f;
    d.classId = 2;
    r.detections.push_back(d);
    return r;
}

// Waits for a condition rather than sleeping a fixed time, so the test is not
// flaky on a loaded board.
template <class Predicate>
bool waitFor(Predicate ready, int milliseconds = 2000) {
    for (int i = 0; i < milliseconds; ++i) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

VS_TEST(a_consumer_receives_exactly_what_the_wire_format_says) {
    const std::string path = testSocketPath();
    ::setenv("VISORA_RESULT_SOCKET", path.c_str(), 1);

    auto selected = vision::resultSinkRegistry().select("result-sink", "unix-socket");
    VS_CHECK(selected.ok());
    if (!selected.ok()) return;
    auto sink = std::move(selected.value());

    const core::Status started = sink->start();
    VS_CHECK(started.ok());
    if (!started.ok()) return;

    VS_CHECK(!sink->hasConsumers());  // nothing connected yet

    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    VS_CHECK(client >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    VS_CHECK(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    VS_CHECK(waitFor([&] { return sink->hasConsumers(); }));

    const Result result = fixture();
    sink->publish(result);

    // Decode the way the Python consumer does.
    std::uint8_t header[4];
    VS_CHECK(recvExact(client, header, 4));
    const std::uint32_t totalLength = readU32(header);

    std::vector<std::uint8_t> body(totalLength);
    VS_CHECK(recvExact(client, body.data(), body.size()));

    const std::uint32_t jsonLength = readU32(body.data());
    VS_CHECK(jsonLength + 4u <= totalLength);

    const std::string json(reinterpret_cast<const char*>(body.data() + 4), jsonLength);
    VS_CHECK(json == vision::wire::toJson(result));
    VS_CHECK(json.find("\"cameraId\":\"cam-1\"") != std::string::npos);
    VS_CHECK(json.find("\"fullJpegSize\":7") != std::string::npos);

    // Everything after the JSON is the JPEG, and nothing else.
    const std::size_t jpegOffset = 4 + jsonLength;
    VS_CHECK_EQ(body.size() - jpegOffset, result.fullJpeg.size());
    VS_CHECK(std::memcmp(body.data() + jpegOffset, result.fullJpeg.data(),
                         result.fullJpeg.size()) == 0);

    ::close(client);
    sink->stop();
    ::unlink(path.c_str());
}

VS_TEST(publishing_with_no_consumer_is_harmless) {
    const std::string path = testSocketPath() + ".idle";
    ::setenv("VISORA_RESULT_SOCKET", path.c_str(), 1);

    auto selected = vision::resultSinkRegistry().select("result-sink", "unix-socket");
    VS_CHECK(selected.ok());
    if (!selected.ok()) return;
    auto sink = std::move(selected.value());
    VS_CHECK(sink->start().ok());

    // Results are produced whether or not anyone is listening; dropping them
    // must cost nothing and must not throw.
    for (int i = 0; i < 100; ++i) sink->publish(fixture());
    VS_CHECK(!sink->hasConsumers());

    sink->stop();
    ::unlink(path.c_str());
}

VS_TEST(a_stale_socket_file_does_not_prevent_starting) {
    // A crash leaves the socket file behind. Refusing to start after an
    // unclean shutdown would need manual intervention on every board.
    const std::string path = testSocketPath() + ".stale";
    ::setenv("VISORA_RESULT_SOCKET", path.c_str(), 1);

    auto first = vision::resultSinkRegistry().select("result-sink", "unix-socket");
    VS_CHECK(first.ok());
    if (!first.ok()) return;
    VS_CHECK(first.value()->start().ok());
    // Deliberately abandon it without stop(), leaving the file in place.
    auto leaked = std::move(first.value());

    auto second = vision::resultSinkRegistry().select("result-sink", "unix-socket");
    VS_CHECK(second.ok());
    if (!second.ok()) return;
    const core::Status started = second.value()->start();
    VS_CHECK(started.ok());

    second.value()->stop();
    leaked->stop();
    ::unlink(path.c_str());
}

VS_TEST(the_sink_set_fans_out_and_reports_what_started) {
    const std::string path = testSocketPath() + ".set";
    ::setenv("VISORA_RESULT_SOCKET", path.c_str(), 1);

    vision::ResultSinkSet set;
    const std::size_t started = set.startAll();
    VS_CHECK(started >= 1);

    const std::vector<std::string> ids = set.activeIds();
    bool found = false;
    for (const std::string& id : ids) {
        if (id == "unix-socket") found = true;
    }
    VS_CHECK(found);

    VS_CHECK(!set.anyConsumers());
    set.publish(fixture());  // nobody listening; must be a no-op, not a crash

    set.stopAll();
    ::unlink(path.c_str());
}

VS_MAIN()
