// The live-stream runtime: the join of a stored camera to its running pipeline,
// and the three verbs that change it.
//
// No GStreamer, no RTSP server, no camera on the network. StreamControl is a
// port precisely so this can be a fake, and SnapshotGrabber is an interface for
// the same reason — the alternative is a test that needs a camera, which is a
// test nobody runs.

#include "TestHarness.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "media/camera/CameraRuntime.hpp"
#include "store/InMemoryCameraRepository.hpp"

using namespace visora;
using visora::media::Camera;
using visora::media::CameraChanges;
using visora::media::CameraRuntime;
using visora::media::CameraService;
using visora::media::CameraState;
using visora::media::Codec;
using visora::media::SnapshotOptions;
using visora::media::StreamControl;
using visora::media::StreamStatus;

namespace {

// Records what it was asked to do and reports whatever the test sets up.
class FakeStreams : public StreamControl {
public:
    std::map<std::string, StreamStatus> live;
    std::vector<std::string> started;
    std::vector<std::string> stopped;
    std::vector<std::string> restarted;

    std::map<std::string, StreamStatus> statuses() const override { return live; }

    std::optional<StreamStatus> statusOf(const std::string& id) const override {
        const auto it = live.find(id);
        if (it == live.end()) return std::nullopt;
        return it->second;
    }

    void startStream(const std::string& id) override { started.push_back(id); }
    void stopStream(const std::string& id) override { stopped.push_back(id); }
    void restartStream(const std::string& id) override { restarted.push_back(id); }
};

class FakeGrabber : public media::SnapshotGrabber {
public:
    std::string lastUrl;
    core::Result<std::vector<std::uint8_t>> answer = std::vector<std::uint8_t>{0xff, 0xd8, 0xff};

    core::Result<std::vector<std::uint8_t>> grab(const std::string& url,
                                                 const SnapshotOptions&) override {
        lastUrl = url;
        return answer;
    }
};

struct Fixture {
    std::shared_ptr<store::InMemoryCameraRepository> repository =
        std::make_shared<store::InMemoryCameraRepository>();
    std::shared_ptr<CameraService> cameras =
        std::make_shared<CameraService>(repository);
    std::shared_ptr<FakeStreams> streams = std::make_shared<FakeStreams>();
    std::shared_ptr<FakeGrabber> grabber = std::make_shared<FakeGrabber>();

    CameraRuntime makeRuntime() { return CameraRuntime(cameras, streams, grabber); }

    std::string addCamera(const std::string& name, const std::string& url) {
        CameraChanges changes;
        changes.name = name;
        changes.rtsp = url;
        auto created = cameras->create(changes);
        VS_CHECK(created.ok());
        return created.ok() ? created.value().id : std::string();
    }
};

}  // namespace

VS_TEST(a_live_session_overrides_the_stored_state) {
    Fixture f;
    const std::string id = f.addCamera("front", "rtsp://cam/1");

    // The row says offline — nothing has run yet.
    auto runtime = f.makeRuntime();
    auto before = runtime.statusOf(id);
    VS_CHECK(before.ok());
    VS_CHECK(before.value().state == CameraState::Offline);
    VS_CHECK(!before.value().streaming);

    StreamStatus online;
    online.state = CameraState::Online;
    online.desired = true;
    online.codec = Codec::H265;
    online.outputRtsp = "rtsp://host:8554/cameras/" + id;
    f.streams->live[id] = online;

    auto after = runtime.statusOf(id);
    VS_CHECK(after.ok());
    VS_CHECK(after.value().state == CameraState::Online);
    VS_CHECK(after.value().codec == Codec::H265);
    VS_CHECK(after.value().streaming);
    VS_CHECK(after.value().outputRtsp == online.outputRtsp);
    // Configuration still comes from the row.
    VS_CHECK(after.value().name == "front");
    VS_CHECK(after.value().inputRtsp == "rtsp://cam/1");
}

VS_TEST(statuses_lists_every_camera_including_ones_with_no_session) {
    Fixture f;
    const std::string a = f.addCamera("a", "rtsp://cam/a");
    const std::string b = f.addCamera("b", "rtsp://cam/b");

    StreamStatus error;
    error.state = CameraState::Error;
    error.lastError = "Could not open resource";
    error.retryCount = 3;
    f.streams->live[a] = error;

    auto runtime = f.makeRuntime();
    auto all = runtime.statuses();
    VS_CHECK(all.ok());
    VS_CHECK_EQ(all.value().size(), std::size_t{2});

    for (const auto& status : all.value()) {
        if (status.id == a) {
            VS_CHECK(status.streaming);
            VS_CHECK(status.state == CameraState::Error);
            VS_CHECK(status.lastError == "Could not open resource");
            VS_CHECK_EQ(status.retryCount, 3);
        } else {
            VS_CHECK(status.id == b);
            VS_CHECK(!status.streaming);
        }
    }
}

VS_TEST(a_stopped_camera_reports_streaming_false_while_still_having_a_session) {
    Fixture f;
    const std::string id = f.addCamera("front", "rtsp://cam/1");

    StreamStatus stopped;              // what StreamManager reports after stop()
    stopped.state = CameraState::Offline;
    stopped.desired = false;
    f.streams->live[id] = stopped;

    auto runtime = f.makeRuntime();
    auto status = runtime.statusOf(id);
    VS_CHECK(status.ok());
    // Not streaming, and not because anything failed: a UI offers "start".
    VS_CHECK(!status.value().streaming);
    VS_CHECK(status.value().state == CameraState::Offline);
    VS_CHECK(status.value().lastError.empty());
}

VS_TEST(the_three_verbs_reach_the_streaming_layer) {
    Fixture f;
    const std::string id = f.addCamera("front", "rtsp://cam/1");
    auto runtime = f.makeRuntime();

    VS_CHECK(runtime.start(id).ok());
    VS_CHECK(runtime.stop(id).ok());
    VS_CHECK(runtime.restart(id).ok());

    VS_CHECK_EQ(f.streams->started.size(), std::size_t{1});
    VS_CHECK_EQ(f.streams->stopped.size(), std::size_t{1});
    VS_CHECK_EQ(f.streams->restarted.size(), std::size_t{1});
    VS_CHECK(f.streams->started.front() == id);
}

VS_TEST(a_verb_on_an_unknown_camera_is_not_found) {
    Fixture f;
    auto runtime = f.makeRuntime();

    auto started = runtime.start("no-such-camera");
    VS_CHECK(!started.ok());
    VS_CHECK(started.error().code == core::ErrorCode::NotFound);

    // Nothing reached the streaming layer: a typo must not create a session.
    VS_CHECK(f.streams->started.empty());
}

VS_TEST(snapshot_reads_the_camera_directly_not_our_restream) {
    Fixture f;
    const std::string id = f.addCamera("front", "rtsp://cam/1");

    StreamStatus online;
    online.state = CameraState::Online;
    online.outputRtsp = "rtsp://host:8554/cameras/" + id;
    f.streams->live[id] = online;

    auto runtime = f.makeRuntime();
    auto jpeg = runtime.snapshot(id);
    VS_CHECK(jpeg.ok());
    VS_CHECK_EQ(jpeg.value().size(), std::size_t{3});
    // The most useful moment to look at a camera is when the restream is
    // broken, so the source URL is what gets opened.
    VS_CHECK(f.grabber->lastUrl == "rtsp://cam/1");
}

VS_TEST(snapshot_without_a_grabber_is_unsupported_not_a_crash) {
    Fixture f;
    const std::string id = f.addCamera("front", "rtsp://cam/1");

    CameraRuntime runtime(f.cameras, f.streams, nullptr);
    auto jpeg = runtime.snapshot(id);
    VS_CHECK(!jpeg.ok());
    VS_CHECK(jpeg.error().code == core::ErrorCode::Unsupported);
}

VS_TEST(the_grabbers_own_error_reaches_the_caller) {
    Fixture f;
    const std::string id = f.addCamera("front", "rtsp://cam/1");
    f.grabber->answer = core::hardwareFailure("timed out waiting for a camera frame");

    auto runtime = f.makeRuntime();
    auto jpeg = runtime.snapshot(id);
    VS_CHECK(!jpeg.ok());
    VS_CHECK(jpeg.error().code == core::ErrorCode::HardwareFailure);
    VS_CHECK(jpeg.error().message == "timed out waiting for a camera frame");
}

VS_MAIN()
