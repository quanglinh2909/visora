#include "store/PostgresRecordingRepository.hpp"

#include <unordered_map>

#include "oatpp-postgresql/Executor.hpp"
#include "oatpp/core/Types.hpp"
#include "oatpp/orm/Executor.hpp"

#include "core/Log.hpp"
#include "core/Time.hpp"
#include "store/RowReader.hpp"

namespace visora::store {
namespace {

constexpr const char* kCategory = "store";

// One column list, one row reader. They drifted apart in the predecessor, where
// every query wrote its own.
constexpr const char* kSegmentColumns =
    "id, camera_id, path, start_at, end_at, duration_ms, codec, container, "
    "recording_mode, has_motion, motion_event_id, status, session_start";

constexpr const char* kEventColumns =
    "id, camera_id, start_at, end_at, max_score, cells, grid_x, grid_y, image_path";

std::string text(const oatpp::String& value) { return value ? *value : std::string(); }

// Instants cross this boundary as epoch MILLISECONDS, not as formatted text.
//
// The columns are TIMESTAMPTZ, and PostgreSQL renders one in the SESSION's time
// zone — so the same row read by two connections with different settings gives
// two different strings. An epoch has no such setting. It is also what the
// domain uses, so nothing is parsed on the way in or out.
//
// CAST(...), never `x::bigint`. oatpp's SQL template parser reads `:name` as a
// bound parameter and, on `::`, consumes the first colon and parses the second
// as one — so `x::bigint` asks for a parameter called "bigint" and the query
// dies with "Parameter not found". Quoted strings are skipped by that parser, so
// a colon in a literal is safe; a cast operator is not.
std::string epochMs(const char* column, const char* alias) {
    return std::string("CAST(EXTRACT(EPOCH FROM ") + column + ") * 1000 AS BIGINT) AS " + alias;
}

// The SELECT lists. Written beside the row readers below, because both are
// positional and drift silently if they are not.
std::string segmentSelect() {
    return "CAST(id AS TEXT), CAST(camera_id AS TEXT), path, " +
           epochMs("start_at", "start_ms") + ", " + epochMs("end_at", "end_ms") +
           ", duration_ms, codec, container, recording_mode, has_motion, "
           "COALESCE(CAST(motion_event_id AS TEXT), ''), status, session_start";
}

std::string eventSelect() {
    return "CAST(id AS TEXT), CAST(camera_id AS TEXT), " + epochMs("start_at", "start_ms") +
           ", " + epochMs("end_at", "end_ms") +
           ", max_score, cells, grid_x, grid_y, image_path";
}

media::RecordingSegment readSegment(const oatpp::Vector<oatpp::Any>& row) {
    const RowReader read(row);
    const auto str = [&read](std::size_t i) { return read.str(i); };
    const auto integer = [&read](std::size_t i) { return read.integer(i); };
    const auto bigint = [&read](std::size_t i) { return read.bigint(i); };
    const auto boolean = [&read](std::size_t i) { return read.boolean(i); };

    media::RecordingSegment segment;
    segment.id = str(0);
    segment.cameraId = str(1);
    segment.path = str(2);
    segment.startMs = bigint(3);
    segment.endMs = bigint(4);
    segment.durationMs = integer(5);
    segment.codec = media::codecFromString(str(6));
    segment.container = str(7);
    segment.recordingMode = media::recordingModeFromString(str(8));
    segment.hasMotion = boolean(9);
    segment.motionEventId = str(10);
    segment.status = media::segmentStatusFromString(str(11));
    segment.sessionStartMs = bigint(12);
    return segment;
}

media::MotionEvent readEvent(const oatpp::Vector<oatpp::Any>& row) {
    const RowReader read(row);
    const auto str = [&read](std::size_t i) { return read.str(i); };
    const auto integer = [&read](std::size_t i) { return read.integer(i); };
    const auto bigint = [&read](std::size_t i) { return read.bigint(i); };
    const auto real = [&read](std::size_t i) { return read.real(i); };

    media::MotionEvent event;
    event.id = str(0);
    event.cameraId = str(1);
    event.startMs = bigint(2);
    // A NULL end_at means the event is still open, and the reader gives 0 for a
    // null — which is exactly what the domain uses to mean "still open".
    event.endMs = bigint(3);
    event.maxScore = real(4);
    event.cells = str(5);
    event.gridX = integer(6);
    event.gridY = integer(7);
    event.imagePath = str(8);
    return event;
}

core::Error toError(const std::shared_ptr<oatpp::orm::QueryResult>& result) {
    const auto message = result ? result->getErrorMessage() : oatpp::String();
    return core::internalError("database: " + text(message));
}

// to_timestamp takes seconds, so the parameter is milliseconds divided by a
// thousand — as a double, because a segment boundary lands on a fraction.
constexpr const char* kFromMs = "to_timestamp(%s / 1000.0)";

std::string fromMs(const char* parameter) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), kFromMs, parameter);
    return buffer;
}

}  // namespace

struct PostgresRecordingRepository::Impl {
    std::shared_ptr<oatpp::orm::Executor> executor;

    std::shared_ptr<oatpp::orm::QueryResult> run(
        const std::string& sql,
        const std::unordered_map<oatpp::String, oatpp::Void>& params) {
        return executor->execute(sql, params);
    }
};

PostgresRecordingRepository::PostgresRecordingRepository(
    std::shared_ptr<oatpp::orm::Executor> executor)
    : m_impl(new Impl{std::move(executor)}) {}

PostgresRecordingRepository::~PostgresRecordingRepository() = default;

core::Result<media::RecordingSegment> PostgresRecordingRepository::upsertSegment(
    const media::RecordingSegment& segment) {
    if (segment.path.empty()) return core::invalidArgument("a segment needs a path");

    // ON CONFLICT (path), because path is what the writer knows: splitmuxsink
    // reports a file opening and later closing, and the two messages share only
    // the name. It also makes a duplicate "opened" — which a pipeline restart
    // produces — harmless instead of a second row for one file.
    //
    // has_motion is OR-ed rather than overwritten: whichever of the two writes
    // learned about the motion, the flag must survive the other.
    const std::string sql =
        std::string(
        "INSERT INTO recording_segments "
        "(camera_id, path, start_at, end_at, duration_ms, codec, container, "
        " recording_mode, has_motion, motion_event_id, status, session_start) ")
        + "VALUES (CAST(:camera_id AS UUID), :path, " + fromMs(":start_ms") + ", " +
        fromMs(":end_ms") + ", :duration_ms, :codec, :container, "
        " :recording_mode, :has_motion, "
        " CASE WHEN :motion_event_id = '' THEN NULL "
        "      ELSE CAST(:motion_event_id AS UUID) END, "
        " :status, :session_start) "
        "ON CONFLICT (path) DO UPDATE SET "
        " end_at = EXCLUDED.end_at, duration_ms = EXCLUDED.duration_ms, "
        " status = EXCLUDED.status, "
        " has_motion = recording_segments.has_motion OR EXCLUDED.has_motion, "
        " motion_event_id = COALESCE(EXCLUDED.motion_event_id, "
        "                            recording_segments.motion_event_id) "
        "RETURNING " + segmentSelect();

    const auto result = m_impl->run(
        sql, {
                 {"camera_id", oatpp::String(segment.cameraId.c_str())},
                 {"path", oatpp::String(segment.path.c_str())},
                 {"start_ms", oatpp::Int64(segment.startMs)},
                 {"end_ms", oatpp::Int64(segment.endMs)},
                 {"duration_ms", oatpp::Int32(segment.durationMs)},
                 {"codec", oatpp::String(media::toString(segment.codec))},
                 {"container", oatpp::String(segment.container.c_str())},
                 {"recording_mode", oatpp::String(media::toString(segment.recordingMode))},
                 {"has_motion", oatpp::Boolean(segment.hasMotion)},
                 {"motion_event_id", oatpp::String(segment.motionEventId.c_str())},
                 {"status", oatpp::String(media::toString(segment.status))},
                 {"session_start", oatpp::Int64(segment.sessionStartMs)},
             });
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::internalError("the segment was not stored");
    return readSegment((*rows)[0]);
}

core::Result<std::vector<media::RecordingSegment>>
PostgresRecordingRepository::segmentsInRange(const std::string& cameraId, std::int64_t fromMs,
                                             std::int64_t toMs) {
    // Half-open on the right, matching segmentsForWindow: a segment that merely
    // touches the boundary does not overlap it.
    const std::string sql = "SELECT " + segmentSelect() +
                            " FROM recording_segments WHERE camera_id = CAST(:camera_id AS UUID)"
                            " AND start_at < " + ::visora::store::fromMs(":to_ms") +
                            " AND end_at > " + ::visora::store::fromMs(":from_ms") +
                            " ORDER BY start_at, id";
    const auto result = m_impl->run(sql, {
                                             {"camera_id", oatpp::String(cameraId.c_str())},
                                             {"from_ms", oatpp::Int64(fromMs)},
                                             {"to_ms", oatpp::Int64(toMs)},
                                         });
    if (!result || !result->isSuccess()) return toError(result);

    std::vector<media::RecordingSegment> out;
    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (rows) {
        out.reserve(rows->size());
        for (const auto& row : *rows) out.push_back(readSegment(row));
    }
    return out;
}

core::Result<media::RecordingSegment> PostgresRecordingRepository::segment(
    const std::string& id) {
    const std::string sql = "SELECT " + segmentSelect() +
                            " FROM recording_segments WHERE id = CAST(:id AS UUID)";
    const auto result = m_impl->run(sql, {{"id", oatpp::String(id.c_str())}});
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::notFound("no recording segment with id " + id);
    return readSegment((*rows)[0]);
}

core::Result<std::vector<media::RecordingSegment>>
PostgresRecordingRepository::segmentsEndingBefore(const std::string& cameraId,
                                                  std::int64_t beforeMs) {
    // 'complete' only: deleting the file a muxer is still writing to ends the
    // recording run in a broken pipeline.
    const std::string sql = "SELECT " + segmentSelect() +
                            " FROM recording_segments WHERE camera_id = CAST(:camera_id AS UUID)"
                            " AND status = 'complete'"
                            " AND end_at < " + ::visora::store::fromMs(":before_ms") +
                            " ORDER BY end_at";
    const auto result = m_impl->run(sql, {
                                             {"camera_id", oatpp::String(cameraId.c_str())},
                                             {"before_ms", oatpp::Int64(beforeMs)},
                                         });
    if (!result || !result->isSuccess()) return toError(result);

    std::vector<media::RecordingSegment> out;
    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (rows) {
        out.reserve(rows->size());
        for (const auto& row : *rows) out.push_back(readSegment(row));
    }
    return out;
}

core::Status PostgresRecordingRepository::removeSegments(const std::vector<std::string>& ids) {
    if (ids.empty()) return {};

    // One statement for the whole batch. Retention can expire thousands of
    // segments at once, and a round trip each would take minutes.
    std::string list;
    std::unordered_map<oatpp::String, oatpp::Void> params;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const std::string name = "id" + std::to_string(i);
        if (i != 0) list += ", ";
        list += "CAST(:" + name + " AS UUID)";
        params[oatpp::String(name.c_str())] = oatpp::String(ids[i].c_str());
    }

    const auto result =
        m_impl->run("DELETE FROM recording_segments WHERE id IN (" + list + ")", params);
    if (!result || !result->isSuccess()) return toError(result);
    return {};
}

core::Result<media::MotionEvent> PostgresRecordingRepository::insertMotionEvent(
    const media::MotionEvent& event) {
    const std::string sql =
        std::string(
        "INSERT INTO motion_events (camera_id, start_at, end_at, max_score, cells, "
        " grid_x, grid_y, image_path) ")
        + "VALUES (CAST(:camera_id AS UUID), " + fromMs(":start_ms") + ", "
        " CASE WHEN :end_ms = 0 THEN NULL ELSE " + fromMs(":end_ms") + " END, "
        " :max_score, :cells, :grid_x, :grid_y, :image_path) "
        "RETURNING " + eventSelect();

    const auto result = m_impl->run(
        sql, {
                 {"camera_id", oatpp::String(event.cameraId.c_str())},
                 {"start_ms", oatpp::Int64(event.startMs)},
                 {"end_ms", oatpp::Int64(event.endMs)},
                 {"max_score", oatpp::Float64(event.maxScore)},
                 {"cells", oatpp::String(event.cells.c_str())},
                 {"grid_x", oatpp::Int32(event.gridX)},
                 {"grid_y", oatpp::Int32(event.gridY)},
                 {"image_path", oatpp::String(event.imagePath.c_str())},
             });
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::internalError("the motion event was not stored");
    return readEvent((*rows)[0]);
}

core::Status PostgresRecordingRepository::closeMotionEvent(const std::string& id,
                                                           std::int64_t endMs, double maxScore,
                                                           const std::string& cells) {
    const auto result = m_impl->run(
        "UPDATE motion_events SET end_at = " + fromMs(":end_ms") +
            ", max_score = :max_score, cells = :cells WHERE id = CAST(:id AS UUID)",
        {
            {"id", oatpp::String(id.c_str())},
            {"end_ms", oatpp::Int64(endMs)},
            {"max_score", oatpp::Float64(maxScore)},
            {"cells", oatpp::String(cells.c_str())},
        });
    if (!result || !result->isSuccess()) return toError(result);
    return {};
}

core::Status PostgresRecordingRepository::setMotionEventImage(const std::string& id,
                                                              const std::string& imagePath) {
    const auto result = m_impl->run(
        "UPDATE motion_events SET image_path = :image_path WHERE id = CAST(:id AS UUID)",
        {
            {"id", oatpp::String(id.c_str())},
            {"image_path", oatpp::String(imagePath.c_str())},
        });
    if (!result || !result->isSuccess()) return toError(result);
    return {};
}

core::Result<std::vector<media::MotionEvent>>
PostgresRecordingRepository::motionEventsInRange(const std::string& cameraId,
                                                 std::int64_t fromMs, std::int64_t toMs) {
    // COALESCE on end_at so an event that is still open matches the window it
    // is happening in rather than being invisible until it finishes.
    const std::string sql = "SELECT " + eventSelect() +
                            " FROM motion_events WHERE camera_id = CAST(:camera_id AS UUID)"
                            " AND start_at < " + ::visora::store::fromMs(":to_ms") +
                            " AND COALESCE(end_at, " + ::visora::store::fromMs(":to_ms") + ")"
                            "     > " + ::visora::store::fromMs(":from_ms") +
                            " ORDER BY start_at";
    const auto result = m_impl->run(sql, {
                                             {"camera_id", oatpp::String(cameraId.c_str())},
                                             {"from_ms", oatpp::Int64(fromMs)},
                                             {"to_ms", oatpp::Int64(toMs)},
                                         });
    if (!result || !result->isSuccess()) return toError(result);

    std::vector<media::MotionEvent> out;
    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (rows) {
        out.reserve(rows->size());
        for (const auto& row : *rows) out.push_back(readEvent(row));
    }
    return out;
}

core::Result<media::MotionEvent> PostgresRecordingRepository::motionEvent(
    const std::string& id) {
    const std::string sql =
        "SELECT " + eventSelect() + " FROM motion_events WHERE id = CAST(:id AS UUID)";
    const auto result = m_impl->run(sql, {{"id", oatpp::String(id.c_str())}});
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::notFound("no motion event with id " + id);
    return readEvent((*rows)[0]);
}

}  // namespace visora::store
