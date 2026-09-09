#include "store/PostgresCameraRepository.hpp"

#include "oatpp-postgresql/ConnectionProvider.hpp"
#include "oatpp-postgresql/Executor.hpp"
#include "oatpp/core/Types.hpp"
#include "oatpp/orm/Executor.hpp"

#include <unordered_map>

#include "core/Log.hpp"
#include "store/RowReader.hpp"

namespace visora::store {
namespace {

constexpr const char* kCategory = "store";

// Every column, in one place and in one order, so the SELECT and the row reader
// cannot drift apart. They did in the predecessor, where each query listed its
// own columns.
// id is cast to text explicitly: PostgreSQL hands a UUID column back as a UUID,
// and oatpp::Any::retrieve<String>() on one THROWS — which, on an HTTP worker
// thread, ends the process. See store/RowReader.hpp.
//
// CAST(id AS TEXT), never `id::text`. oatpp's SQL template parser reads `:name`
// as a bound parameter, and on `::` it consumes the first colon and then parses
// the second as one — so `id::text` asks for a parameter called "text" and the
// query dies with "Parameter not found". It skips quoted strings, so a colon
// inside a literal is safe; a cast operator is not.
constexpr const char* kColumns =
    "CAST(id AS TEXT), name, rtsp, state, input_rtsp, output_rtsp, codec, hardware, "
    "recording_enabled, recording_mode, motion_enabled, motion_sensitivity, "
    "motion_threshold, pre_motion_seconds, post_motion_seconds, segment_seconds, "
    "motion_keyframe_only, motion_grid_x, motion_grid_y, motion_cell_levels, "
    "motion_zones, motion_save_events, retention_days, "
    "retry_count, last_error, last_changed_at";

std::string text(const oatpp::String& value) { return value ? *value : std::string(); }

media::Camera readRow(const oatpp::Vector<oatpp::Any>& row) {
    const RowReader read(row);
    const auto str = [&read](std::size_t i) { return read.str(i); };
    const auto boolean = [&read](std::size_t i) { return read.boolean(i); };
    const auto integer = [&read](std::size_t i) { return read.integer(i); };
    const auto real = [&read](std::size_t i) { return read.real(i); };

    media::Camera camera;
    camera.id = str(0);
    camera.name = str(1);
    camera.rtsp = str(2);
    camera.state = media::cameraStateFromString(str(3));
    camera.inputRtsp = str(4);
    camera.outputRtsp = str(5);
    camera.codec = media::codecFromString(str(6));
    camera.hardware = str(7);
    camera.recordingEnabled = boolean(8);
    camera.recordingMode = media::recordingModeFromString(str(9));
    camera.motionEnabled = boolean(10);
    camera.motionSensitivity = real(11);
    camera.motionThreshold = real(12);
    camera.preMotionSeconds = integer(13);
    camera.postMotionSeconds = integer(14);
    camera.segmentSeconds = integer(15);
    camera.motionKeyframeOnly = boolean(16);
    camera.motionGridX = integer(17);
    camera.motionGridY = integer(18);
    camera.motionCellLevels = str(19);
    camera.motionZones = str(20);
    camera.motionSaveEvents = boolean(21);
    camera.retentionDays = integer(22);
    camera.retryCount = integer(23);
    camera.lastError = str(24);
    camera.lastChangedAt = str(25);
    return camera;
}

// Turns a failed oatpp query into an Error that carries the database's own
// message. The predecessor logged these and returned a bare false, so a
// constraint violation reached the operator as "internal error".
core::Error toError(const std::shared_ptr<oatpp::orm::QueryResult>& result) {
    const auto message = result ? result->getErrorMessage() : oatpp::String();
    return core::internalError("database: " + text(message));
}

}  // namespace

struct PostgresCameraRepository::Impl {
    std::shared_ptr<oatpp::orm::Executor> executor;

    // oatpp's ORM builds prepared statements from a DbClient subclass with
    // macros. Plain execute() is used instead: the queries are few, they are
    // right here next to the row reader, and the macro form would put the
    // column list in a third place.
    std::shared_ptr<oatpp::orm::QueryResult> run(const std::string& sql,
                                                 const std::unordered_map<
                                                     oatpp::String, oatpp::Void>& params) {
        return executor->execute(sql, params);
    }
};

PostgresCameraRepository::PostgresCameraRepository(
    std::shared_ptr<oatpp::orm::Executor> executor)
    : m_impl(new Impl{std::move(executor)}) {}

PostgresCameraRepository::~PostgresCameraRepository() = default;

core::Result<std::vector<media::Camera>> PostgresCameraRepository::list() {
    const auto result = m_impl->run(
        std::string("SELECT ") + kColumns + " FROM cameras ORDER BY name, id", {});
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    std::vector<media::Camera> cameras;
    if (rows) {
        cameras.reserve(rows->size());
        for (const auto& row : *rows) cameras.push_back(readRow(row));
    }
    return cameras;
}

core::Result<media::Camera> PostgresCameraRepository::get(const std::string& id) {
    const auto result = m_impl->run(
        std::string("SELECT ") + kColumns + " FROM cameras WHERE id = CAST(:id AS UUID)",
        {{"id", oatpp::String(id.c_str())}});
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::notFound("no camera with id " + id);
    return readRow((*rows)[0]);
}

core::Result<media::Camera> PostgresCameraRepository::insert(const media::Camera& camera) {
    const auto result = m_impl->run(
        std::string(
            "INSERT INTO cameras (name, rtsp, input_rtsp, hardware, recording_enabled, "
            "recording_mode, motion_enabled, motion_sensitivity, motion_threshold, "
            "pre_motion_seconds, post_motion_seconds, segment_seconds, motion_keyframe_only, "
            "motion_grid_x, motion_grid_y, motion_cell_levels, motion_zones, "
            "motion_save_events, retention_days) "
            "VALUES (:name, :rtsp, :input_rtsp, :hardware, :recording_enabled, "
            ":recording_mode, :motion_enabled, :motion_sensitivity, :motion_threshold, "
            ":pre_motion_seconds, :post_motion_seconds, :segment_seconds, "
            ":motion_keyframe_only, :motion_grid_x, :motion_grid_y, "
            ":motion_cell_levels, :motion_zones, :motion_save_events, "
            ":retention_days) RETURNING ") +
            kColumns,
        {
            {"name", oatpp::String(camera.name.c_str())},
            {"rtsp", oatpp::String(camera.rtsp.c_str())},
            {"input_rtsp", oatpp::String(camera.inputRtsp.c_str())},
            {"hardware", oatpp::String(camera.hardware.c_str())},
            {"recording_enabled", oatpp::Boolean(camera.recordingEnabled)},
            {"recording_mode", oatpp::String(media::toString(camera.recordingMode))},
            {"motion_enabled", oatpp::Boolean(camera.motionEnabled)},
            {"motion_sensitivity", oatpp::Float64(camera.motionSensitivity)},
            {"motion_threshold", oatpp::Float64(camera.motionThreshold)},
            {"pre_motion_seconds", oatpp::Int32(camera.preMotionSeconds)},
            {"post_motion_seconds", oatpp::Int32(camera.postMotionSeconds)},
            {"segment_seconds", oatpp::Int32(camera.segmentSeconds)},
            {"motion_keyframe_only", oatpp::Boolean(camera.motionKeyframeOnly)},
            {"motion_grid_x", oatpp::Int32(camera.motionGridX)},
            {"motion_grid_y", oatpp::Int32(camera.motionGridY)},
            {"motion_cell_levels", oatpp::String(camera.motionCellLevels.c_str())},
            {"motion_zones", oatpp::String(camera.motionZones.c_str())},
            {"motion_save_events", oatpp::Boolean(camera.motionSaveEvents)},
            {"retention_days", oatpp::Int32(camera.retentionDays)},
        });
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::internalError("insert returned no row");
    return readRow((*rows)[0]);
}

core::Result<media::Camera> PostgresCameraRepository::update(const media::Camera& camera) {
    // Runtime columns are deliberately absent: updateRuntime() owns those, so
    // an operator's edit cannot overwrite a state the pipeline just reported.
    const auto result = m_impl->run(
        std::string("UPDATE cameras SET name = :name, rtsp = :rtsp, input_rtsp = :input_rtsp, "
                    "hardware = :hardware, recording_enabled = :recording_enabled, "
                    "recording_mode = :recording_mode, motion_enabled = :motion_enabled, "
                    "motion_sensitivity = :motion_sensitivity, "
                    "motion_threshold = :motion_threshold, "
                    "pre_motion_seconds = :pre_motion_seconds, "
                    "post_motion_seconds = :post_motion_seconds, "
                    "segment_seconds = :segment_seconds, "
                    "motion_keyframe_only = :motion_keyframe_only, "
                    "motion_grid_x = :motion_grid_x, motion_grid_y = :motion_grid_y, "
                    "motion_cell_levels = :motion_cell_levels, "
                    "motion_zones = :motion_zones, "
                    "motion_save_events = :motion_save_events, "
                    "retention_days = :retention_days "
                    "WHERE id = CAST(:id AS UUID) RETURNING ") +
            kColumns,
        {
            {"id", oatpp::String(camera.id.c_str())},
            {"name", oatpp::String(camera.name.c_str())},
            {"rtsp", oatpp::String(camera.rtsp.c_str())},
            {"input_rtsp", oatpp::String(camera.inputRtsp.c_str())},
            {"hardware", oatpp::String(camera.hardware.c_str())},
            {"recording_enabled", oatpp::Boolean(camera.recordingEnabled)},
            {"recording_mode", oatpp::String(media::toString(camera.recordingMode))},
            {"motion_enabled", oatpp::Boolean(camera.motionEnabled)},
            {"motion_sensitivity", oatpp::Float64(camera.motionSensitivity)},
            {"motion_threshold", oatpp::Float64(camera.motionThreshold)},
            {"pre_motion_seconds", oatpp::Int32(camera.preMotionSeconds)},
            {"post_motion_seconds", oatpp::Int32(camera.postMotionSeconds)},
            {"segment_seconds", oatpp::Int32(camera.segmentSeconds)},
            {"motion_keyframe_only", oatpp::Boolean(camera.motionKeyframeOnly)},
            {"motion_grid_x", oatpp::Int32(camera.motionGridX)},
            {"motion_grid_y", oatpp::Int32(camera.motionGridY)},
            {"motion_cell_levels", oatpp::String(camera.motionCellLevels.c_str())},
            {"motion_zones", oatpp::String(camera.motionZones.c_str())},
            {"motion_save_events", oatpp::Boolean(camera.motionSaveEvents)},
            {"retention_days", oatpp::Int32(camera.retentionDays)},
        });
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::notFound("no camera with id " + camera.id);
    return readRow((*rows)[0]);
}

core::Status PostgresCameraRepository::remove(const std::string& id) {
    const auto result = m_impl->run("DELETE FROM cameras WHERE id = CAST(:id AS UUID)",
                                    {{"id", oatpp::String(id.c_str())}});
    if (!result || !result->isSuccess()) return toError(result);
    return {};
}

core::Status PostgresCameraRepository::updateRuntime(const std::string& id,
                                                     const media::CameraRuntimeFields& fields) {
    // last_changed_at comes from the caller, not from the database's now().
    // Two adapters that stamp their own time make the same domain operation
    // observably different depending on where the row happens to live.
    const auto result = m_impl->run(
        "UPDATE cameras SET state = :state, codec = :codec, output_rtsp = :output_rtsp, "
        "retry_count = :retry_count, last_error = :last_error, "
        "last_changed_at = :last_changed_at "
        "WHERE id = CAST(:id AS UUID)",
        {
            {"id", oatpp::String(id.c_str())},
            {"state", oatpp::String(media::toString(fields.state))},
            {"codec", oatpp::String(media::toString(fields.codec))},
            {"output_rtsp", oatpp::String(fields.outputRtsp.c_str())},
            {"retry_count", oatpp::Int32(fields.retryCount)},
            {"last_error", oatpp::String(fields.lastError.c_str())},
            {"last_changed_at", oatpp::String(fields.lastChangedAt.c_str())},
        });
    if (!result || !result->isSuccess()) return toError(result);
    return {};
}

core::Result<std::shared_ptr<oatpp::orm::Executor>> makePostgresExecutor(const std::string& url,
                                                                        int maxConnections,
                                                                        int idleSeconds) {
    try {
        auto provider = std::make_shared<oatpp::postgresql::ConnectionProvider>(url.c_str());
        auto pool = oatpp::postgresql::ConnectionPool::createShared(
            provider, static_cast<v_int64>(maxConnections),
            std::chrono::seconds(idleSeconds));
        VS_INFO(kCategory) << "postgres pool ready (max " << maxConnections << " connections)";
        return std::static_pointer_cast<oatpp::orm::Executor>(
            std::make_shared<oatpp::postgresql::Executor>(pool));
    } catch (const std::exception& error) {
        return core::internalError(std::string("postgres connection failed: ") + error.what());
    }
}

}  // namespace visora::store
