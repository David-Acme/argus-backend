#include "reminder-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace reminder_query;

drogon::Task<std::optional<ReminderSchema>>
ReminderRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return ReminderSchema(result.front());
}

drogon::Task<std::vector<ReminderSchema>>
ReminderRepository::findByTargetUser(int64_t targetUserId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_TARGET.data(), targetUserId);

  std::vector<ReminderSchema> data;
  for (const auto& row : result)
    data.push_back(ReminderSchema(row));
  co_return data;
}

drogon::Task<ReminderSchema>
ReminderRepository::create(const ReminderCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(),
                                   input.createdBy ? *input.createdBy
                                                   : std::optional<int64_t>{},
                                   input.targetUserId, input.title,
                                   input.description, input.scheduledAt,
                                   input.recurrenceRule
                                       ? *input.recurrenceRule
                                       : std::optional<std::string>{});

  ReminderSchema schema;
  schema.id = result.insertId();
  schema.createdBy = input.createdBy;
  schema.targetUserId = input.targetUserId;
  schema.title = input.title;
  schema.description = input.description;
  schema.scheduledAt = input.scheduledAt;
  schema.recurrenceRule = input.recurrenceRule;
  schema.isCompleted = false;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<ReminderSchema>
ReminderRepository::update(int64_t id, const ReminderUpdateInput& input) const
{
  auto client = DbService::client();
  std::string sql = UPDATE_PREFIX.data();
  std::vector<std::string> args;

  auto addString = [&](std::string_view column,
                       const std::optional<std::string>& value) {
    if (!value)
      return;
    if (!args.empty())
      sql += ", ";
    sql += column;
    args.push_back(*value);
  };

  addString(UPDATE_COL_TITLE, input.title);
  addString(UPDATE_COL_DESCRIPTION, input.description);
  if (input.scheduledAt) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_SCHEDULED_AT;
    args.push_back(std::to_string(*input.scheduledAt));
  }
  addString(UPDATE_COL_RECURRENCE_RULE, input.recurrenceRule);
  if (input.isCompleted) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_IS_COMPLETED;
    args.push_back(*input.isCompleted ? "1" : "0");
  }
  if (input.completedAt) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_COMPLETED_AT;
    args.push_back(std::to_string(*input.completedAt));
  }

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "Reminder not found for update";
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
    LOG_WARN << "Reminder not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> ReminderRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
ReminderRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();

  const auto [query, args] =
      sync_query::buildSyncQuery(filter, FIND, FIND_FROM, FIND_ALL);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(ReminderSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
ReminderRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::client();

  const auto [query, args] =
      sync_query::buildSyncQuery(filter, FIND_DELETED, FIND_DELETED_FROM, FIND_DELETED_ALL);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(ReminderSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>> ReminderRepository::findLast() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST.data());
  if (result.empty())
    co_return std::nullopt;
  co_return ReminderSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
ReminderRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty())
    co_return std::nullopt;
  co_return ReminderSchema(result.front()).toJson();
}
