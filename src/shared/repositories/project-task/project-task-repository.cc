#include "project-task-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace project_task_query;

drogon::Task<std::optional<ProjectTaskSchema>>
ProjectTaskRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectTaskSchema(result.front());
}

drogon::Task<std::vector<ProjectTaskSchema>>
ProjectTaskRepository::findByProject(int64_t projectId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_PROJECT.data(), projectId);

  std::vector<ProjectTaskSchema> data;
  for (const auto& row : result)
    data.push_back(ProjectTaskSchema(row));
  co_return data;
}

drogon::Task<ProjectTaskSchema>
ProjectTaskRepository::create(const ProjectTaskCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.projectId,
      input.createdBy ? *input.createdBy : std::optional<int64_t>{},
      input.assigneeId ? *input.assigneeId : std::optional<int64_t>{},
      input.title, input.status, input.priority,
      input.dueAt ? *input.dueAt : std::optional<int64_t>{}, input.sortOrder);

  ProjectTaskSchema schema;
  schema.id = result.insertId();
  schema.projectId = input.projectId;
  schema.createdBy = input.createdBy;
  schema.assigneeId = input.assigneeId;
  schema.title = input.title;
  schema.status = input.status;
  schema.priority = input.priority;
  schema.dueAt = input.dueAt;
  schema.sortOrder = input.sortOrder;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<ProjectTaskSchema>
ProjectTaskRepository::update(int64_t id,
                              const ProjectTaskUpdateInput& input) const
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

  addString(UPDATE_COL_TITLE, input.title);
  addString(UPDATE_COL_STATUS, input.status);
  addString(UPDATE_COL_PRIORITY, input.priority);
  addInt(UPDATE_COL_ASSIGNEE_ID, input.assigneeId);
  addInt(UPDATE_COL_DUE_AT, input.dueAt);
  if (input.sortOrder)
    addColumn(UPDATE_COL_SORT_ORDER, std::to_string(*input.sortOrder));

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "ProjectTask not found for update";
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
    LOG_WARN << "ProjectTask not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> ProjectTaskRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
ProjectTaskRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::readOnlyClient();
  const auto [query, args] = sync_query::withUser(
      sync_query::buildSyncQuery(filter, FIND, FIND_FROM, FIND_ALL, FIND_AFTER,
                                 FIND_AFTER_FROM),
      filter.userId, OWNERSHIP_PLACEHOLDERS);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(ProjectTaskSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
ProjectTaskRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::readOnlyClient();
  const auto [query, args] = sync_query::withUser(
      sync_query::buildSyncQuery(filter, FIND_DELETED, FIND_DELETED_FROM,
                                 FIND_DELETED_ALL, FIND_DELETED_AFTER,
                                 FIND_DELETED_AFTER_FROM),
      filter.userId, OWNERSHIP_PLACEHOLDERS);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(ProjectTaskSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>> ProjectTaskRepository::findLast(const SyncFilter& filter) const
{
  auto client = DbService::readOnlyClient();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectTaskSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
ProjectTaskRepository::findLastDeleted(const SyncFilter& filter) const
{
  auto client = DbService::readOnlyClient();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_DELETED.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return ProjectTaskSchema(result.front()).toJson();
}
