// Camera business rules, tested against an in-memory repository.
//
// Milliseconds, no database, no container, no fixture teardown. The predecessor
// could test none of this: every path ran through oatpp-postgresql, so the
// rules were only ever exercised by hand against a live system.

#include "TestHarness.hpp"

#include <memory>
#include <string>

#include "media/camera/CameraService.hpp"
#include "store/InMemoryCameraRepository.hpp"

using namespace visora;
using visora::media::Camera;
using visora::media::CameraChanges;
using visora::media::CameraDiff;
using visora::media::CameraService;
using visora::media::CameraState;
using visora::media::RecordingMode;

namespace {

struct Fixture {
    std::shared_ptr<store::InMemoryCameraRepository> repository =
        std::make_shared<store::InMemoryCameraRepository>();
    // What the streaming layer would have been told.
    std::vector<std::string> added;
    std::vector<std::pair<std::string, CameraDiff>> changed;
    std::vector<std::string> removed;

    CameraService makeService() {
        media::CameraEvents events;
        events.added = [this](const Camera& c) { added.push_back(c.id); };
        events.changed = [this](const Camera& c, const CameraDiff& d) {
            changed.emplace_back(c.id, d);
        };
        events.removed = [this](const std::string& id) { removed.push_back(id); };
        return CameraService(repository, std::move(events));
    }
};

CameraChanges validCreate(const char* name = "Front door") {
    CameraChanges changes;
    changes.name = name;
    changes.rtsp = "rtsp://10.0.0.1/stream";
    return changes;
}

}  // namespace

VS_TEST(creating_a_camera_stores_it_and_announces_it) {
    Fixture fixture;
    CameraService service = fixture.makeService();

    auto created = service.create(validCreate());
    VS_CHECK(created.ok());
    if (!created.ok()) return;

    VS_CHECK(!created.value().id.empty());
    VS_CHECK(created.value().name == "Front door");
    // inputRtsp tracks rtsp on create, or the pipeline has no source.
    VS_CHECK(created.value().inputRtsp == "rtsp://10.0.0.1/stream");
    VS_CHECK(created.value().state == CameraState::Offline);

    VS_CHECK_EQ(fixture.added.size(), static_cast<std::size_t>(1));
    VS_CHECK(fixture.added[0] == created.value().id);
}

VS_TEST(a_camera_without_a_name_or_source_is_rejected) {
    Fixture fixture;
    CameraService service = fixture.makeService();

    CameraChanges noName = validCreate();
    noName.name.reset();
    auto a = service.create(noName);
    VS_CHECK(!a.ok());
    VS_CHECK(a.error().code == core::ErrorCode::InvalidArgument);

    CameraChanges noRtsp = validCreate();
    noRtsp.rtsp.reset();
    auto b = service.create(noRtsp);
    VS_CHECK(!b.ok());
    VS_CHECK(b.error().code == core::ErrorCode::InvalidArgument);

    // Nothing was stored and nothing was announced.
    VS_CHECK(fixture.added.empty());
    auto all = service.list();
    VS_CHECK(all.ok());
    if (all.ok()) VS_CHECK(all.value().empty());
}

VS_TEST(an_http_url_is_rejected_because_it_would_never_connect) {
    Fixture fixture;
    CameraService service = fixture.makeService();
    CameraChanges changes = validCreate();
    changes.rtsp = "http://10.0.0.1/stream.m3u8";
    auto created = service.create(changes);
    VS_CHECK(!created.ok());
    VS_CHECK(created.error().message.find("rtsp://") != std::string::npos);
}

VS_TEST(out_of_range_settings_are_rejected_with_the_field_named) {
    Fixture fixture;
    CameraService service = fixture.makeService();

    CameraChanges changes = validCreate();
    changes.motionSensitivity = 1.5;
    auto a = service.create(changes);
    VS_CHECK(!a.ok());
    VS_CHECK(a.error().message.find("motionSensitivity") != std::string::npos);

    changes = validCreate();
    changes.segmentSeconds = 0;  // splitmuxsink would cut continuously
    auto b = service.create(changes);
    VS_CHECK(!b.ok());
    VS_CHECK(b.error().message.find("segmentSeconds") != std::string::npos);
}

VS_TEST(renaming_a_camera_does_not_disturb_its_stream) {
    // The distinction that matters: a cosmetic edit must not tear down a live
    // pipeline. The predecessor rebuilt on any update.
    Fixture fixture;
    CameraService service = fixture.makeService();
    auto created = service.create(validCreate());
    VS_CHECK(created.ok());
    if (!created.ok()) return;

    CameraChanges rename;
    rename.name = "Back door";
    auto updated = service.update(created.value().id, rename);
    VS_CHECK(updated.ok());
    if (updated.ok()) VS_CHECK(updated.value().name == "Back door");

    VS_CHECK(fixture.changed.empty());  // streaming layer never told
}

VS_TEST(changing_the_source_or_hardware_does_disturb_it) {
    Fixture fixture;
    CameraService service = fixture.makeService();
    auto created = service.create(validCreate());
    VS_CHECK(created.ok());
    if (!created.ok()) return;

    CameraChanges moved;
    moved.rtsp = "rtsp://10.0.0.2/other";
    auto updated = service.update(created.value().id, moved);
    VS_CHECK(updated.ok());
    if (updated.ok()) {
        // inputRtsp must follow, or the pipeline keeps the old address.
        VS_CHECK(updated.value().inputRtsp == "rtsp://10.0.0.2/other");
    }

    VS_CHECK_EQ(fixture.changed.size(), static_cast<std::size_t>(1));
    if (!fixture.changed.empty()) {
        VS_CHECK(fixture.changed[0].second.sourceChanged);
        VS_CHECK(!fixture.changed[0].second.recordingChanged);
    }

    CameraChanges hardware;
    hardware.hardware = "vaapi";
    VS_CHECK(service.update(created.value().id, hardware).ok());
    VS_CHECK_EQ(fixture.changed.size(), static_cast<std::size_t>(2));
    if (fixture.changed.size() >= 2) VS_CHECK(fixture.changed[1].second.sourceChanged);
}

VS_TEST(setting_a_field_to_its_current_value_changes_nothing) {
    // An idempotent PUT from a UI that resends the whole object must not
    // restart a stream.
    Fixture fixture;
    CameraService service = fixture.makeService();
    auto created = service.create(validCreate());
    VS_CHECK(created.ok());
    if (!created.ok()) return;

    CameraChanges same;
    same.rtsp = "rtsp://10.0.0.1/stream";  // unchanged
    same.name = "Front door";              // unchanged
    VS_CHECK(service.update(created.value().id, same).ok());
    VS_CHECK(fixture.changed.empty());
}

VS_TEST(recording_and_motion_changes_are_reported_separately) {
    Fixture fixture;
    CameraService service = fixture.makeService();
    auto created = service.create(validCreate());
    VS_CHECK(created.ok());
    if (!created.ok()) return;

    CameraChanges recording;
    recording.recordingEnabled = true;
    recording.recordingMode = RecordingMode::Motion;
    VS_CHECK(service.update(created.value().id, recording).ok());
    VS_CHECK_EQ(fixture.changed.size(), static_cast<std::size_t>(1));
    if (!fixture.changed.empty()) {
        VS_CHECK(fixture.changed[0].second.recordingChanged);
        VS_CHECK(!fixture.changed[0].second.sourceChanged);
    }

    CameraChanges motion;
    motion.motionEnabled = true;
    VS_CHECK(service.update(created.value().id, motion).ok());
    if (fixture.changed.size() >= 2) {
        VS_CHECK(fixture.changed[1].second.motionChanged);
        VS_CHECK(!fixture.changed[1].second.recordingChanged);
    }
}

VS_TEST(operating_on_an_unknown_camera_is_not_found_not_a_silent_success) {
    Fixture fixture;
    CameraService service = fixture.makeService();

    auto got = service.get("00000000-0000-4000-8000-999999999999");
    VS_CHECK(!got.ok());
    VS_CHECK(got.error().code == core::ErrorCode::NotFound);

    const core::Status removed = service.remove("00000000-0000-4000-8000-999999999999");
    VS_CHECK(!removed.ok());
    VS_CHECK(removed.error().code == core::ErrorCode::NotFound);
    VS_CHECK(fixture.removed.empty());

    CameraChanges changes;
    changes.name = "x";
    auto updated = service.update("00000000-0000-4000-8000-999999999999", changes);
    VS_CHECK(!updated.ok());
    VS_CHECK(updated.error().code == core::ErrorCode::NotFound);
}

VS_TEST(removing_a_camera_announces_it_only_after_the_row_is_gone) {
    Fixture fixture;
    CameraService service = fixture.makeService();
    auto created = service.create(validCreate());
    VS_CHECK(created.ok());
    if (!created.ok()) return;
    const std::string id = created.value().id;

    VS_CHECK(service.remove(id).ok());
    VS_CHECK_EQ(fixture.removed.size(), static_cast<std::size_t>(1));

    // Gone for good: a restart must not resurrect a deleted camera.
    auto got = service.get(id);
    VS_CHECK(!got.ok());
    VS_CHECK(got.error().code == core::ErrorCode::NotFound);
}

VS_TEST(runtime_updates_do_not_touch_operator_edited_fields) {
    // Runtime state changes on every reconnect. It must not collide with an
    // operator editing the same row.
    Fixture fixture;
    CameraService service = fixture.makeService();
    auto created = service.create(validCreate());
    VS_CHECK(created.ok());
    if (!created.ok()) return;
    const std::string id = created.value().id;

    media::CameraRuntimeFields fields;
    fields.state = CameraState::Online;
    fields.codec = media::Codec::H265;
    fields.outputRtsp = "rtsp://host:8554/cameras/x";
    fields.retryCount = 3;
    VS_CHECK(service.reportRuntime(id, fields).ok());

    auto got = service.get(id);
    VS_CHECK(got.ok());
    if (!got.ok()) return;
    VS_CHECK(got.value().state == CameraState::Online);
    VS_CHECK(got.value().codec == media::Codec::H265);
    VS_CHECK_EQ(got.value().retryCount, 3);
    // Untouched.
    VS_CHECK(got.value().name == "Front door");
    VS_CHECK(got.value().rtsp == "rtsp://10.0.0.1/stream");
}

VS_TEST(legacy_state_strings_from_older_rows_still_read) {
    // Rows written before the runtime states were collapsed to three still hold
    // the old detailed values.
    VS_CHECK(media::cameraStateFromString("running") == CameraState::Online);
    VS_CHECK(media::cameraStateFromString("auth_error") == CameraState::Error);
    VS_CHECK(media::cameraStateFromString("unsupported_codec") == CameraState::Error);
    VS_CHECK(media::cameraStateFromString("something-new") == CameraState::Offline);
    VS_CHECK(media::cameraStateFromString("ONLINE") == CameraState::Online);
}

VS_TEST(recording_mode_parsing_accepts_what_the_database_holds) {
    VS_CHECK(media::recordingModeFromString("motion") == RecordingMode::Motion);
    VS_CHECK(media::recordingModeFromString("continuous") == RecordingMode::Continuous);
    VS_CHECK(media::recordingModeFromString("always") == RecordingMode::Continuous);
    VS_CHECK(media::recordingModeFromString("off") == RecordingMode::Off);
    VS_CHECK(media::recordingModeFromString("") == RecordingMode::Off);
}

VS_TEST(runtime_state_is_stamped_by_the_service_not_by_the_adapter) {
    Fixture fixture;
    CameraService service = fixture.makeService();
    auto created = service.create(validCreate());
    VS_CHECK(created.ok());
    if (!created.ok()) return;
    const std::string id = created.value().id;

    media::CameraRuntimeFields fields;
    fields.state = CameraState::Online;
    fields.codec = media::Codec::H264;
    fields.outputRtsp = "rtsp://host:8554/cameras/" + id;
    VS_CHECK(service.reportRuntime(id, fields).ok());

    auto stored = service.get(id);
    VS_CHECK(stored.ok());
    VS_CHECK(stored.value().state == CameraState::Online);
    // The Postgres adapter used to invent this with now() and the in-memory one
    // left it empty, so the same operation was observably different depending
    // on where the row happened to live.
    const std::string& stamp = stored.value().lastChangedAt;
    VS_CHECK_EQ(stamp.size(), std::size_t{20});   // 2026-09-09T12:59:07Z
    VS_CHECK(!stamp.empty() && stamp.back() == 'Z');
    VS_CHECK(stamp.find('T') == 10);

    // A caller that already has a timestamp keeps it, so the value stored and
    // the value pushed over the websocket are the same instant.
    fields.lastChangedAt = "2020-01-01T00:00:00Z";
    VS_CHECK(service.reportRuntime(id, fields).ok());
    VS_CHECK(service.get(id).value().lastChangedAt == "2020-01-01T00:00:00Z");
}

VS_MAIN()
