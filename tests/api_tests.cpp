// The HTTP adapter layer: DTO mapping and error translation.
//
// No server and no database. What is tested here is the part that is easy to
// get subtly wrong — which fields a partial update touches, and which fields a
// client is allowed to set at all.

#include "TestHarness.hpp"

#include <string>

#include "api/HttpError.hpp"
#include "api/mappers/CameraMapper.hpp"

using namespace visora;
using visora::api::CameraDto;

VS_TEST(a_camera_maps_to_the_wire_shape_the_ui_expects) {
    media::Camera camera;
    camera.id = "abc";
    camera.name = "Front door";
    camera.rtsp = "rtsp://10.0.0.1/stream";
    camera.state = media::CameraState::Online;
    camera.codec = media::Codec::H265;
    camera.recordingMode = media::RecordingMode::Motion;
    camera.motionEnabled = true;
    camera.motionGridX = 16;

    const auto dto = api::toDto(camera);
    VS_CHECK(dto->id && *dto->id == "abc");
    VS_CHECK(dto->state && *dto->state == "online");
    VS_CHECK(dto->codec && *dto->codec == "h265");
    VS_CHECK(dto->recordingMode && *dto->recordingMode == "motion");
    VS_CHECK(dto->motionEnabled && *dto->motionEnabled);
    VS_CHECK(dto->motionGridX && *dto->motionGridX == 16);
}

VS_TEST(an_omitted_field_is_not_a_change) {
    // The bug this prevents: the predecessor could not tell "field absent" from
    // "field set to false", so a PUT that only renamed a camera silently turned
    // off recording and motion detection.
    auto dto = CameraDto::createShared();
    dto->name = "Back door";

    const media::CameraChanges changes = api::toChanges(dto);
    VS_CHECK(changes.name.has_value());
    VS_CHECK(!changes.rtsp.has_value());
    VS_CHECK(!changes.motionEnabled.has_value());
    VS_CHECK(!changes.recordingEnabled.has_value());
    VS_CHECK(!changes.recordingMode.has_value());
    VS_CHECK(!changes.segmentSeconds.has_value());
}

VS_TEST(a_field_explicitly_set_to_false_is_a_change) {
    // The other half: false must be distinguishable from absent, or a client
    // could never turn anything off.
    auto dto = CameraDto::createShared();
    dto->motionEnabled = false;

    const media::CameraChanges changes = api::toChanges(dto);
    VS_CHECK(changes.motionEnabled.has_value());
    if (changes.motionEnabled.has_value()) VS_CHECK(changes.motionEnabled.value() == false);
}

VS_TEST(server_owned_fields_cannot_be_set_by_a_client) {
    // A UI that reads a camera, edits one field and PUTs the whole object back
    // must not be able to declare the camera online or rewrite its error.
    auto dto = CameraDto::createShared();
    dto->id = "someone-elses-id";
    dto->state = "online";
    dto->codec = "h264";
    dto->outputRtsp = "rtsp://attacker/";
    dto->retryCount = 999;
    dto->lastError = "";
    dto->lastChangedAt = "2000-01-01T00:00:00Z";
    dto->name = "legitimate rename";

    const media::CameraChanges changes = api::toChanges(dto);
    // Only the rename survives; CameraChanges has no field for the rest.
    VS_CHECK(changes.name.has_value());
    VS_CHECK(changes.name.value() == "legitimate rename");
}

VS_TEST(a_null_dto_maps_to_no_changes_rather_than_crashing) {
    // An empty body reaches the controller as a null object.
    const media::CameraChanges changes = api::toChanges(nullptr);
    VS_CHECK(!changes.name.has_value());
    VS_CHECK(!changes.rtsp.has_value());
}

VS_TEST(domain_errors_map_to_the_status_a_client_can_act_on) {
    VS_CHECK(api::httpStatusFor(core::ErrorCode::InvalidArgument).code == 400);
    VS_CHECK(api::httpStatusFor(core::ErrorCode::NotFound).code == 404);
    VS_CHECK(api::httpStatusFor(core::ErrorCode::Internal).code == 500);
    // 503, not 501: the request is reasonable and may well work on a machine
    // with the right hardware, which is exactly the case on a board with no NPU.
    VS_CHECK(api::httpStatusFor(core::ErrorCode::Unsupported).code == 503);
    VS_CHECK(api::httpStatusFor(core::ErrorCode::HardwareFailure).code == 503);
}

VS_TEST(recording_mode_round_trips_through_the_wire) {
    for (const auto mode : {media::RecordingMode::Off, media::RecordingMode::Continuous,
                            media::RecordingMode::Motion}) {
        media::Camera camera;
        camera.recordingMode = mode;
        const auto dto = api::toDto(camera);
        const media::CameraChanges changes = api::toChanges(dto);
        VS_CHECK(changes.recordingMode.has_value());
        if (changes.recordingMode.has_value()) VS_CHECK(changes.recordingMode.value() == mode);
    }
}

VS_MAIN()
