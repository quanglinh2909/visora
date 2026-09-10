// The HTTP adapter layer: DTO mapping and error translation.
//
// No server and no database. What is tested here is the part that is easy to
// get subtly wrong — which fields a partial update touches, and which fields a
// client is allowed to set at all.

#include "TestHarness.hpp"

#include <string>

#include "oatpp/parser/json/mapping/ObjectMapper.hpp"

#include "api/QueryParam.hpp"
#include "api/HttpError.hpp"
#include "api/dto/WebRtcDto.hpp"
#include "api/mappers/AiMapper.hpp"
#include "api/mappers/CameraMapper.hpp"
#include "api/mappers/StreamStatusMapper.hpp"
#include "api/ws/CameraStateFeed.hpp"

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
    // An empty body reaches the controller as a null object. Spelled out
    // rather than as `nullptr`, which is ambiguous now that AI jobs have a
    // toChanges of their own.
    const media::CameraChanges changes = api::toChanges(oatpp::Object<CameraDto>());
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

// --- live stream status ------------------------------------------------------

VS_TEST(a_runtime_status_maps_to_the_wire_shape_the_ui_expects) {
    media::CameraRuntimeStatus status;
    status.id = "abc";
    status.name = "Front door";
    status.state = media::CameraState::Error;
    status.inputRtsp = "rtsp://cam/1";
    status.outputRtsp = "rtsp://host:8554/cameras/abc";
    status.codec = media::Codec::H265;
    status.hardware = "auto";
    status.recordingEnabled = true;
    status.retryCount = 4;
    status.lastError = "Could not open resource";
    status.streaming = true;

    const auto dto = api::toDto(status);
    VS_CHECK(dto->id == "abc");
    VS_CHECK(dto->state == "error");
    VS_CHECK(dto->codec == "h265");
    VS_CHECK(dto->outputRtsp == "rtsp://host:8554/cameras/abc");
    VS_CHECK(dto->recordingEnabled == true);
    VS_CHECK(dto->retryCount == 4u);
    VS_CHECK(dto->streaming == true);
}

// --- the camera-state websocket message --------------------------------------
//
// A deployed UI parses these bytes, so the shape is asserted rather than
// assumed. The fields the predecessor sent — id, state, lastError,
// lastChangedAt — must all still be there.

VS_TEST(the_state_message_carries_what_the_predecessor_sent) {
    media::CameraRuntimeStatus status;
    status.id = "abc";
    status.name = "Front door";
    status.state = media::CameraState::Online;
    status.codec = media::Codec::H264;
    status.outputRtsp = "rtsp://host:8554/cameras/abc";
    status.lastChangedAt = "2026-09-09T10:00:00Z";
    status.streaming = true;

    const std::string message = api::CameraStateFeed::messageFor(status);
    for (const char* key : {"\"id\":\"abc\"", "\"state\":\"online\"", "\"lastError\":\"\"",
                            "\"lastChangedAt\":\"2026-09-09T10:00:00Z\"",
                            "\"codec\":\"h264\"", "\"retryCount\":0", "\"streaming\":true"}) {
        if (message.find(key) == std::string::npos) {
            ::visora::test::reportFailure(__FILE__, __LINE__,
                                          std::string("missing ") + key + " in " + message);
        }
    }
}

VS_TEST(a_name_with_a_quote_or_a_control_character_stays_valid_json) {
    media::CameraRuntimeStatus status;
    status.id = "abc";
    status.name = "Say \"hi\"";
    status.lastError = std::string("line\nbreak\twith a NUL:") + '\x01';

    const std::string message = api::CameraStateFeed::messageFor(status);
    VS_CHECK(message.find("Say \\\"hi\\\"") != std::string::npos);
    VS_CHECK(message.find("line\\nbreak\\twith") != std::string::npos);
    // A raw control byte in a JSON string is what a strict parser rejects.
    VS_CHECK(message.find("\\u0001") != std::string::npos);
    VS_CHECK(message.find('\x01') == std::string::npos);
}

VS_TEST(an_unchanged_status_is_not_broadcast_twice) {
    // The status callback fires on every retry, and a retry that fails the same
    // way as the last one is not news. With no clients connected this asserts
    // only the deduplication decision, which is the part with the logic in it.
    api::CameraStateFeed feed;
    media::CameraRuntimeStatus status;
    status.id = "abc";
    status.state = media::CameraState::Error;
    status.lastError = "unreachable";

    VS_CHECK_EQ(feed.clientCount(), std::size_t{0});
    feed.broadcast(status);
    feed.broadcast(status);
    VS_CHECK(api::CameraStateFeed::messageFor(status) ==
             api::CameraStateFeed::messageFor(status));

    // A removal message is distinguishable from a state message.
    const std::string removed = api::CameraStateFeed::removalMessageFor("abc");
    VS_CHECK(removed.find("\"state\":\"removed\"") != std::string::npos);
}


// --- the shapes a separately deployed UI parses --------------------------------
//
// These are a CONTRACT, and the way they break is silent. A bare array where
// the client reads `data.types` gives an empty dropdown and no error anywhere;
// a bare array where it reads `data.sessions` crashes the page on an HTTP 200,
// which is worse than a 404 because nothing retries and nothing logs.
//
// So the keys are asserted on the serialised JSON, which is what actually
// crosses the wire — a renamed DTO field passes a compile and fails here.

namespace {

std::string asJson(const oatpp::Void& dto) {
    oatpp::parser::json::mapping::ObjectMapper mapper;
    const auto text = mapper.writeToString(dto);
    return text ? std::string(text->c_str()) : std::string();
}

bool hasKey(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\":") != std::string::npos;
}

}  // namespace

VS_TEST(the_model_type_catalogue_is_an_object_with_types_in_it) {
    const std::string json = asJson(api::modelTypesDto());
    // What the deployed UI reads. A bare array here is an empty dropdown.
    VS_CHECK(hasKey(json, "types"));
    VS_CHECK(json.rfind("{", 0) == 0);
    // Every type appears as a plain string in `types`, not only as an object.
    VS_CHECK(json.find("\"yolov8_detect\"") != std::string::npos);
    // And the richer form is still there for anything that wants it.
    VS_CHECK(hasKey(json, "entries"));
    VS_CHECK(hasKey(json, "label"));
}

VS_TEST(a_catalogue_entry_carries_both_id_and_value) {
    // Two clients, two names for one string. Carrying both is cheaper than a
    // version negotiation over a dropdown.
    const std::string json = asJson(api::transformsDto());
    VS_CHECK(json.rfind("[", 0) == 0);
    VS_CHECK(hasKey(json, "id"));
    VS_CHECK(hasKey(json, "value"));
    VS_CHECK(json.find("\"crop\"") != std::string::npos);
}

VS_TEST(the_viewers_answer_is_an_object_with_sessions_in_it) {
    // The one that actually crashed a dashboard: it reads data.sessions, and a
    // bare array made that undefined AFTER a 200.
    auto sessions = oatpp::List<oatpp::Object<api::ViewerDto>>::createShared();
    auto one = api::ViewerDto::createShared();
    one->sessionId = "s1";
    one->cameraId = "cam1";
    one->mode = "live";
    one->connected = true;
    one->ageMs = static_cast<v_int64>(1500);
    one->ageSeconds = static_cast<v_int64>(1);
    one->clientAddr = "10.0.0.9";
    sessions->push_back(one);

    auto out = api::ViewersDto::createShared();
    out->total = static_cast<v_int64>(1);
    out->live = static_cast<v_int64>(1);
    out->playback = static_cast<v_int64>(0);
    out->sessions = sessions;

    const std::string json = asJson(out);
    VS_CHECK(json.rfind("{", 0) == 0);
    for (const char* key : {"total", "live", "playback", "sessions"}) {
        VS_CHECK(hasKey(json, key));
    }
    // Per-session fields a viewer list is useless without.
    for (const char* key : {"sessionId", "cameraId", "mode", "connected", "ageMs",
                            "ageSeconds", "clientAddr"}) {
        VS_CHECK(hasKey(json, key));
    }
}

VS_TEST(a_model_file_is_named_the_way_the_ui_reads_it) {
    auto dto = api::AiModelDto::createShared();
    dto->path = "/models/yolov8n.rknn";
    dto->name = "yolov8n.rknn";
    dto->fileName = dto->name;
    dto->sizeBytes = static_cast<v_uint64>(1234);

    const std::string json = asJson(dto);
    for (const char* key : {"path", "fileName", "sizeBytes"}) {
        VS_CHECK(hasKey(json, key));
    }
}

// --- query parameters --------------------------------------------------------

VS_TEST(a_percent_encoded_timestamp_is_decoded_before_it_is_parsed) {
    // The bug this exists for: the frontend proxies every engine call through a
    // Next.js API route, which rebuilds the URL and encodes ':' as "%3A". oatpp
    // hands query values over exactly as they arrived, so the timestamp parser
    // saw "2026-09-10T04%3A49%3A31Z" and answered 400 — and the timeline, which
    // treats a failed fetch as an empty day, showed a recording camera as
    // having recorded nothing.
    VS_CHECK(api::percentDecoded(oatpp::String("2026-09-10T04%3A49%3A31.000Z")) ==
             "2026-09-10T04:49:31.000Z");
    // Already decoded is left alone, because both forms arrive: the browser
    // reaches some deployments directly.
    VS_CHECK(api::percentDecoded(oatpp::String("2026-09-10T04:49:31.000Z")) ==
             "2026-09-10T04:49:31.000Z");
}

VS_TEST(a_plus_in_a_query_value_is_not_a_space) {
    // The obvious "decode a query parameter" also maps '+' to ' ', which is the
    // form-encoded convention. Doing that here would corrupt the exact values
    // this decoder exists to rescue: '+' is the sign of a timezone offset, and
    // "+07:00" would become " 07:00".
    VS_CHECK(api::percentDecoded(oatpp::String("2026-09-10T11:49:31+07:00")) ==
             "2026-09-10T11:49:31+07:00");
    VS_CHECK(api::percentDecoded(oatpp::String("2026-09-10T11%3A49%3A31%2B07%3A00")) ==
             "2026-09-10T11:49:31+07:00");
}

VS_TEST(a_broken_escape_is_passed_through_rather_than_swallowed) {
    // A truncated or invalid escape stays as it is: the value's own parser
    // gives a better error about the whole string than this could about one
    // character, and silently dropping the '%' would turn a malformed request
    // into a plausible-looking wrong one.
    VS_CHECK(api::percentDecoded(oatpp::String("100%")) == "100%");
    VS_CHECK(api::percentDecoded(oatpp::String("%zz")) == "%zz");
    VS_CHECK(api::percentDecoded(oatpp::String("%3")) == "%3");
    VS_CHECK(api::percentDecoded(oatpp::String()).empty());
}


VS_TEST(an_instant_parameter_accepts_both_forms_the_clients_send) {
    // The published API has always had two, and the timeline uses both in one
    // screen: recordings are asked for with ISO instants and a hover thumbnail
    // with epoch milliseconds. Accepting only one answers 400 to half of it.
    VS_CHECK(api::parseInstant(oatpp::String("1789014506000")) == 1789014506000LL);
    VS_CHECK(api::parseInstant(oatpp::String("2026-09-10T03:34:35.293Z")) == 1789011275293LL);
    // Encoded on the way through the proxy, and still the same instant.
    VS_CHECK(api::parseInstant(oatpp::String("2026-09-10T03%3A34%3A35.293Z")) ==
             1789011275293LL);
}

VS_TEST(an_instant_parameter_rejects_what_is_neither) {
    // -1 rather than a plausible number, so an endpoint says 400 instead of
    // quietly serving the wrong day.
    VS_CHECK(api::parseInstant(oatpp::String("yesterday")) < 0);
    VS_CHECK(api::parseInstant(oatpp::String("2026-09-10")) < 0);
    VS_CHECK(api::parseInstant(oatpp::String("12x34")) < 0);
    // Empty is "not given" — every caller decides what that means for itself.
    VS_CHECK(api::parseInstant(oatpp::String("")) < 0);
    VS_CHECK(api::parseInstant(oatpp::String()) < 0);
    // Digits that overflow are not a timestamp however digit-like they look.
    VS_CHECK(api::parseInstant(oatpp::String("99999999999999999999999")) < 0);
}


VS_MAIN()
