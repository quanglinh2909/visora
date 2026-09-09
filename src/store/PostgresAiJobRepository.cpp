#include "store/PostgresAiJobRepository.hpp"

#include <unordered_map>

#include "oatpp/core/Types.hpp"
#include "oatpp/orm/Executor.hpp"

#include "core/Log.hpp"
#include "store/AiJobJson.hpp"
#include "store/RowReader.hpp"

namespace visora::store {
namespace {

// The SELECT list, written beside the row reader below because both are
// positional and drift silently if they are not.
//
// CAST(... AS TEXT), never `id::text`. oatpp's SQL template parser reads
// `:name` as a bound parameter and, on `::`, consumes the first colon and
// parses the second as one — so `id::text` asks for a parameter called "text"
// and the query dies with "Parameter not found".
constexpr const char* kJobSelect =
    "CAST(id AS TEXT), name, CAST(camera_id AS TEXT), enabled, max_fps, "
    "CAST(stages AS TEXT)";

std::string text(const oatpp::String& value) { return value ? *value : std::string(); }

vision::AiJob readJob(const oatpp::Vector<oatpp::Any>& row) {
    const RowReader read(row);
    vision::AiJob job;
    job.id = read.str(0);
    job.name = read.str(1);
    job.cameraId = read.str(2);
    job.enabled = read.boolean(3);
    job.maxFps = read.integer(4);
    job.stages = stagesFromJson(read.str(5));
    return job;
}

core::Error toError(const std::shared_ptr<oatpp::orm::QueryResult>& result) {
    const auto message = result ? result->getErrorMessage() : oatpp::String();
    return core::internalError("database: " + text(message));
}

}  // namespace

struct PostgresAiJobRepository::Impl {
    std::shared_ptr<oatpp::orm::Executor> executor;

    std::shared_ptr<oatpp::orm::QueryResult> run(
        const std::string& sql,
        const std::unordered_map<oatpp::String, oatpp::Void>& params) {
        return executor->execute(sql, params);
    }
};

PostgresAiJobRepository::PostgresAiJobRepository(std::shared_ptr<oatpp::orm::Executor> executor)
    : m_impl(new Impl{std::move(executor)}) {}

PostgresAiJobRepository::~PostgresAiJobRepository() = default;

core::Result<std::vector<vision::AiJob>> PostgresAiJobRepository::list() {
    const std::string sql =
        std::string("SELECT ") + kJobSelect + " FROM ai_jobs ORDER BY created_at, id";
    const auto result = m_impl->run(sql, {});
    if (!result || !result->isSuccess()) return toError(result);

    std::vector<vision::AiJob> out;
    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (rows) {
        out.reserve(rows->size());
        for (const auto& row : *rows) out.push_back(readJob(row));
    }
    return out;
}

core::Result<std::vector<vision::AiJob>> PostgresAiJobRepository::listForCamera(
    const std::string& cameraId) {
    const std::string sql = std::string("SELECT ") + kJobSelect +
                            " FROM ai_jobs WHERE camera_id = CAST(:camera_id AS UUID)"
                            " ORDER BY created_at, id";
    const auto result = m_impl->run(sql, {{"camera_id", oatpp::String(cameraId.c_str())}});
    if (!result || !result->isSuccess()) return toError(result);

    std::vector<vision::AiJob> out;
    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (rows) {
        out.reserve(rows->size());
        for (const auto& row : *rows) out.push_back(readJob(row));
    }
    return out;
}

core::Result<vision::AiJob> PostgresAiJobRepository::get(const std::string& id) {
    const std::string sql =
        std::string("SELECT ") + kJobSelect + " FROM ai_jobs WHERE id = CAST(:id AS UUID)";
    const auto result = m_impl->run(sql, {{"id", oatpp::String(id.c_str())}});
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::notFound("no AI job with id " + id);
    return readJob((*rows)[0]);
}

core::Result<vision::AiJob> PostgresAiJobRepository::insert(const vision::AiJob& job) {
    // The id is the database's: gen_random_uuid() on the column. Generating one
    // here would mean two places that decide what a job is called.
    const std::string sql =
        std::string(
            "INSERT INTO ai_jobs (name, camera_id, enabled, max_fps, stages) "
            "VALUES (:name, CAST(:camera_id AS UUID), :enabled, :max_fps, "
            "        CAST(:stages AS JSONB)) RETURNING ") +
        kJobSelect;

    const auto result = m_impl->run(
        sql, {
                 {"name", oatpp::String(job.name.c_str())},
                 {"camera_id", oatpp::String(job.cameraId.c_str())},
                 {"enabled", oatpp::Boolean(job.enabled)},
                 {"max_fps", oatpp::Int32(job.maxFps)},
                 {"stages", oatpp::String(stagesToJson(job.stages).c_str())},
             });
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::internalError("the AI job was not stored");
    return readJob((*rows)[0]);
}

core::Result<vision::AiJob> PostgresAiJobRepository::update(const vision::AiJob& job) {
    // Every column, because the caller has already applied its changes to a
    // whole job — the service reads, edits and writes back. A partial UPDATE
    // here would be a second place that decides what "unchanged" means.
    const std::string sql =
        std::string(
            "UPDATE ai_jobs SET name = :name, camera_id = CAST(:camera_id AS UUID), "
            " enabled = :enabled, max_fps = :max_fps, stages = CAST(:stages AS JSONB) "
            "WHERE id = CAST(:id AS UUID) RETURNING ") +
        kJobSelect;

    const auto result = m_impl->run(
        sql, {
                 {"id", oatpp::String(job.id.c_str())},
                 {"name", oatpp::String(job.name.c_str())},
                 {"camera_id", oatpp::String(job.cameraId.c_str())},
                 {"enabled", oatpp::Boolean(job.enabled)},
                 {"max_fps", oatpp::Int32(job.maxFps)},
                 {"stages", oatpp::String(stagesToJson(job.stages).c_str())},
             });
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::notFound("no AI job with id " + job.id);
    return readJob((*rows)[0]);
}

core::Status PostgresAiJobRepository::remove(const std::string& id) {
    const std::string sql =
        "DELETE FROM ai_jobs WHERE id = CAST(:id AS UUID) RETURNING CAST(id AS TEXT)";
    const auto result = m_impl->run(sql, {{"id", oatpp::String(id.c_str())}});
    if (!result || !result->isSuccess()) return toError(result);

    const auto rows = result->fetch<oatpp::Vector<oatpp::Vector<oatpp::Any>>>();
    if (!rows || rows->empty()) return core::notFound("no AI job with id " + id);
    return {};
}

}  // namespace visora::store
