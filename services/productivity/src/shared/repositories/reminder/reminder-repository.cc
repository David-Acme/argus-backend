#include "reminder-repository.hxx"

#include <ctime>
#include <sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <trantor/utils/Logger.h>
#include <utility>
#include <vector>

using namespace reminder_query;

namespace
{
struct ClientChoice
{
  drogon::orm::DbClientPtr pooled;
  drogon::orm::DbClient* client{nullptr};
};

ClientChoice choose(drogon::orm::DbClient* client)
{
  ClientChoice choice;
  if (client) {
    choice.client = client;
    return choice;
  }
  choice.pooled = DbService::productivityClient();
  choice.client = choice.pooled.get();
  return choice;
}
}

drogon::Task<std::optional<ReminderSchema>>
ReminderRepository::findOwned(const ReminderOwnedInput& input) const
{
  const auto choice = choose(input.client);
  const auto result = co_await choice.client->execSqlCoro(
      std::string(FIND_OWNED), input.id, input.targetUserId);
  if (result.empty())
    co_return std::nullopt;
  co_return ReminderSchema(result.front());
}

drogon::Task<std::vector<ReminderSchema>>
ReminderRepository::findByTargetUser(const ReminderListInput& input) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_BY_TARGET), input.targetUserId,
      input.includeCompleted ? 1 : 0, input.limit);

  std::vector<ReminderSchema> data;
  data.reserve(result.size());
  for (const auto& row : result)
    data.emplace_back(row);
  co_return data;
}

drogon::Task<ReminderSchema>
ReminderRepository::create(const ReminderCreateInput& input) const
{
  const auto choice = choose(input.client);
  const auto result = co_await choice.client->execSqlCoro(
      std::string(INSERT),
      input.createdBy, input.targetUserId, input.title, input.description,
      input.scheduledAt, input.recurrenceRule);

  ReminderSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
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
ReminderRepository::update(const ReminderUpdateInput& input) const
{
  const auto choice = choose(input.client);
  std::string sql(UPDATE_PREFIX);
  std::vector<std::string> args;
  bool changed = false;

  const auto addFragment = [&](std::string_view fragment) {
    if (changed)
      sql += ", ";
    sql += fragment;
    changed = true;
  };
  const auto addColumn = [&](std::string_view column, std::string value) {
    addFragment(column);
    args.push_back(std::move(value));
  };

  if (input.title)
    addColumn(UPDATE_COL_TITLE, *input.title);
  if (input.description)
    addColumn(UPDATE_COL_DESCRIPTION, *input.description);
  if (input.scheduledAt)
    addColumn(UPDATE_COL_SCHEDULED_AT, std::to_string(*input.scheduledAt));
  if (input.isCompleted)
    addColumn(UPDATE_COL_IS_COMPLETED, *input.isCompleted ? "1" : "0");
  if (input.clearCompletedAt)
    addFragment(UPDATE_COL_COMPLETED_AT_NULL);
  else if (input.completedAt)
    addColumn(UPDATE_COL_COMPLETED_AT, std::to_string(*input.completedAt));

  if (!changed) {
    auto existing =
        co_await findOwned({.id = input.id,
                            .targetUserId = input.targetUserId,
                            .client = choice.client});
    if (!existing) {
      LOG_WARN << "Reminder not found for update";
      co_return {};
    }
    co_return *existing;
  }

  sql += UPDATE_SUFFIX;
  args.push_back(std::to_string(input.id));
  args.push_back(std::to_string(input.targetUserId));
  const auto& argsRef = args;
  co_await choice.client->execSqlCoro(sql, argsRef);

  auto updated =
      co_await findOwned({.id = input.id,
                          .targetUserId = input.targetUserId,
                          .client = choice.client});
  if (!updated) {
    LOG_WARN << "Reminder not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool>
ReminderRepository::remove(const ReminderOwnedInput& input) const
{
  const auto choice = choose(input.client);
  const auto result = co_await choice.client->execSqlCoro(
      std::string(REMOVE), input.id, input.targetUserId);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
ReminderRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();

  const auto [query, args] = sync_query::withUser(
      {.parts = sync_query::buildSyncQuery({.filter = filter,
                                            .queryBoth = FIND,
                                            .queryFrom = FIND_FROM,
                                            .queryAll = FIND_ALL,
                                            .queryAfterBoth = FIND_AFTER,
                                            .queryAfterFrom = FIND_AFTER_FROM}),
       .userId = filter.userId,
       .placeholders = OWNERSHIP_PLACEHOLDERS});
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  data.reserve(rows.size());
  for (const auto& row : rows)
    data.push_back(ReminderSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
ReminderRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();

  const auto [query, args] = sync_query::withUser(
      {.parts = sync_query::buildSyncQuery(
           {.filter = filter,
            .queryBoth = FIND_DELETED,
            .queryFrom = FIND_DELETED_FROM,
            .queryAll = FIND_DELETED_ALL,
            .queryAfterBoth = FIND_DELETED_AFTER,
            .queryAfterFrom = FIND_DELETED_AFTER_FROM}),
       .userId = filter.userId,
       .placeholders = OWNERSHIP_PLACEHOLDERS});
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(ReminderSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
ReminderRepository::findLast(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_LAST), filter.userId.value_or(0));
  if (result.empty())
    co_return std::nullopt;
  co_return ReminderSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
ReminderRepository::findLastDeleted(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_LAST_DELETED), filter.userId.value_or(0));
  if (result.empty())
    co_return std::nullopt;
  co_return ReminderSchema(result.front()).toJson();
}
