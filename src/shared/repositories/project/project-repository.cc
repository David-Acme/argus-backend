#include "project-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace project_query;

drogon::Task<std::optional<ProjectSchema>>
ProjectRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectSchema(result.front());
}

drogon::Task<std::vector<ProjectSchema>>
ProjectRepository::findByOwner(int64_t ownerId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_OWNER.data(), ownerId);

  std::vector<ProjectSchema> data;
  for (const auto& row : result)
    data.push_back(ProjectSchema(row));
  co_return data;
}

drogon::Task<ProjectSchema>
ProjectRepository::create(const ProjectCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.ownerId, input.name, input.description, input.status,
      input.color, input.startsAt ? *input.startsAt : std::optional<int64_t>{},
      input.targetAt ? *input.targetAt : std::optional<int64_t>{});

  ProjectSchema schema;
  schema.id = result.insertId();
  schema.ownerId = input.ownerId;
  schema.name = input.name;
  schema.description = input.description;
  schema.status = input.status;
  schema.color = input.color;
  schema.startsAt = input.startsAt;
  schema.targetAt = input.targetAt;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<ProjectSchema>
ProjectRepository::update(int64_t id, const ProjectUpdateInput& input) const
{
  auto client = DbService::client();
  std::string sql = UPDATE_PREFIX.data();
  std::vector<std::string> args;

  const auto addColumn = [&](std::string_view column, std::string value) {
    if (!args.empty())
      sql += ", ";
    sql += column;
    args.push_back(std::move(value));
  };
  const auto addString = [&](std::string_view column,
                             const std::optional<std::string>& value) {
    if (value)
      addColumn(column, *value);
  };
  const auto addInt = [&](std::string_view column,
                          const std::optional<int64_t>& value) {
    if (value)
      addColumn(column, std::to_string(*value));
  };

  addString(UPDATE_COL_NAME, input.name);
  addString(UPDATE_COL_DESCRIPTION, input.description);
  addString(UPDATE_COL_STATUS, input.status);
  addString(UPDATE_COL_COLOR, input.color);
  addInt(UPDATE_COL_STARTS_AT, input.startsAt);
  addInt(UPDATE_COL_TARGET_AT, input.targetAt);

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "Project not found for update";
      co_return {};
    }
    co_return *existing;
  }

  sql += UPDATE_SUFFIX.data();
  args.push_back(std::to_string(id));
  const auto& argsRef = args;
  co_await client->execSqlCoro(sql, argsRef);

  auto updated = co_await findById(id);
  if (!updated) {
    LOG_WARN << "Project not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> ProjectRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
ProjectRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();
  const auto [query, args] = sync_query::withUser(
      sync_query::buildSyncQuery(filter, FIND, FIND_FROM, FIND_ALL, FIND_AFTER,
                                 FIND_AFTER_FROM),
      filter.userId, OWNERSHIP_PLACEHOLDERS);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(ProjectSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
ProjectRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::client();
  const auto [query, args] = sync_query::withUser(
      sync_query::buildSyncQuery(filter, FIND_DELETED, FIND_DELETED_FROM,
                                 FIND_DELETED_ALL, FIND_DELETED_AFTER,
                                 FIND_DELETED_AFTER_FROM),
      filter.userId, OWNERSHIP_PLACEHOLDERS);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(ProjectSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>> ProjectRepository::findLast(const SyncFilter& filter) const
{
  auto client = DbService::client();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>> ProjectRepository::findLastDeleted(const SyncFilter& filter) const
{
  auto client = DbService::client();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_DELETED.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectSchema(result.front()).toJson();
}
