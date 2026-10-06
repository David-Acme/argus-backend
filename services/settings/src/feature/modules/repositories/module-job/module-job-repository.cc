#include "module-job-repository.hxx"

#include <drogon/orm/SqlBinder.h>

#include <string>
#include <utility>

using namespace module_job_query;

namespace
{
ModuleJobSchema jobFrom(const drogon::orm::Row& row)
{
  return {.id = row["id"].as<std::int64_t>(),
          .moduleId = row["module_id"].as<std::string>(),
          .kind = jobKindFromString(row["kind"].as<std::string>()).value_or(JobKind::Install),
          .owner = row["owner"].as<std::string>(),
          .state = jobStateFromString(row["state"].as<std::string>()).value_or(JobState::Failed),
          .reason = row["reason"].as<std::string>(),
          .bytesDone = row["bytes_done"].as<std::int64_t>(),
          .bytesTotal = row["bytes_total"].as<std::int64_t>(),
          .requestedBy = row["requested_by"].as<std::int64_t>(),
          .createdAt = row["created_at"].as<std::int64_t>(),
          .updatedAt = row["updated_at"].as<std::int64_t>(),
          .stateSince = row["state_since"].as<std::int64_t>()};
}
}

ModuleJobRepository::ModuleJobRepository(drogon::orm::DbClientPtr client) : client_(std::move(client)) {}

ModuleJobSchema ModuleJobRepository::create(const ModuleJobCreateInput& input) const
{
  const auto result =
      client_->execSqlSync(INSERT, input.moduleId, std::string(jobKindToString(input.kind)),
                           input.bytesTotal, input.requestedBy, input.at, input.at, input.at);
  return {.id = static_cast<std::int64_t>(result.insertId()),
          .moduleId = input.moduleId,
          .kind = input.kind,
          .owner = {},
          .state = JobState::Queued,
          .reason = {},
          .bytesDone = 0,
          .bytesTotal = input.bytesTotal,
          .requestedBy = input.requestedBy,
          .createdAt = input.at,
          .updatedAt = input.at,
          .stateSince = input.at};
}

std::optional<ModuleJobSchema> ModuleJobRepository::update(std::int64_t id, const ModuleJobUpdateInput& input) const
{
  std::string sql = UPDATE_PREFIX;
  if (input.state)
    sql += UPDATE_COL_STATE;
  if (input.reason)
    sql += UPDATE_COL_REASON;
  if (input.owner)
    sql += UPDATE_COL_OWNER;
  if (input.bytesDone)
    sql += UPDATE_COL_BYTES_DONE;
  if (input.bytesTotal)
    sql += UPDATE_COL_BYTES_TOTAL;
  sql += UPDATE_SUFFIX;
  {
    auto binder = *client_ << sql;
    binder << input.at;
    if (input.state)
      binder << std::string(jobStateToString(*input.state)) << input.at;
    if (input.reason)
      binder << *input.reason;
    if (input.owner)
      binder << *input.owner;
    if (input.bytesDone)
      binder << *input.bytesDone;
    if (input.bytesTotal)
      binder << *input.bytesTotal;
    binder << id;
    binder << drogon::orm::Mode::Blocking;
    binder >> [](const drogon::orm::Result&) {};
    binder.exec();
  }
  return findById(id);
}

std::optional<ModuleJobSchema> ModuleJobRepository::findById(std::int64_t id) const
{
  const auto rows = client_->execSqlSync(std::string(COLUMNS) + WHERE_ID, id);
  if (rows.empty())
    return std::nullopt;
  return jobFrom(rows.front());
}

std::vector<ModuleJobSchema> ModuleJobRepository::select(const std::string& sql) const
{
  const auto rows = client_->execSqlSync(sql);
  std::vector<ModuleJobSchema> jobs;
  jobs.reserve(rows.size());
  for (const auto& row : rows)
    jobs.push_back(jobFrom(row));
  return jobs;
}

std::vector<ModuleJobSchema> ModuleJobRepository::findOpen() const
{
  return select(std::string(COLUMNS) + WHERE_OPEN);
}

std::vector<ModuleJobSchema> ModuleJobRepository::findLatestPerModule() const
{
  return select(std::string(COLUMNS) + WHERE_LATEST);
}
