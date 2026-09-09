#include "calendar-event-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace calendar_event_query;

drogon::Task<std::optional<CalendarEventSchema>>
CalendarEventRepository::findById(int64_t id) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return CalendarEventSchema(result.front());
}

drogon::Task<std::vector<CalendarEventSchema>>
CalendarEventRepository::findByOwnerRange(
    const CalendarEventRangeInput& input) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(
      FIND_BY_OWNER_RANGE.data(), input.ownerId, input.from, input.to);

  std::vector<CalendarEventSchema> data;
  for (const auto& row : result)
    data.push_back(CalendarEventSchema(row));
  co_return data;
}

drogon::Task<CalendarEventSchema>
CalendarEventRepository::create(const CalendarEventCreateInput& input) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(),
      input.createdBy ? *input.createdBy : std::optional<int64_t>{},
      input.ownerId,
      input.projectId ? *input.projectId : std::optional<int64_t>{},
      input.title, input.description, input.location, input.color,
      input.startsAt,
      input.endsAt ? *input.endsAt : std::optional<int64_t>{},
      input.isAllDay ? 1 : 0,
      input.recurrenceRule ? *input.recurrenceRule
                           : std::optional<std::string>{});

  CalendarEventSchema schema;
  schema.id = result.insertId();
  schema.createdBy = input.createdBy;
  schema.ownerId = input.ownerId;
  schema.projectId = input.projectId;
  schema.title = input.title;
  schema.description = input.description;
  schema.location = input.location;
  schema.color = input.color;
  schema.startsAt = input.startsAt;
  schema.endsAt = input.endsAt;
  schema.isAllDay = input.isAllDay;
  schema.recurrenceRule = input.recurrenceRule;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<CalendarEventSchema>
CalendarEventRepository::update(int64_t id,
                                const CalendarEventUpdateInput& input) const
{
  auto client = DbService::productivityClient();
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
  addString(UPDATE_COL_DESCRIPTION, input.description);
  addString(UPDATE_COL_LOCATION, input.location);
  addString(UPDATE_COL_COLOR, input.color);
  addInt(UPDATE_COL_STARTS_AT, input.startsAt);
  addInt(UPDATE_COL_ENDS_AT, input.endsAt);
  if (input.isAllDay)
    addColumn(UPDATE_COL_IS_ALL_DAY, *input.isAllDay ? "1" : "0");
  addString(UPDATE_COL_RECURRENCE_RULE, input.recurrenceRule);
  addInt(UPDATE_COL_PROJECT_ID, input.projectId);

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "CalendarEvent not found for update";
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
    LOG_WARN << "CalendarEvent not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> CalendarEventRepository::remove(int64_t id) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
CalendarEventRepository::find(const SyncFilter& filter) const
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
  for (const auto& row : rows)
    data.push_back(CalendarEventSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
CalendarEventRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto [query, args] = sync_query::withUser(
                                   {.parts = sync_query::buildSyncQuery({.filter = filter,
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
    data.push_back(CalendarEventSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
CalendarEventRepository::findLast(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return CalendarEventSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
CalendarEventRepository::findLastDeleted(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_DELETED.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return CalendarEventSchema(result.front()).toJson();
}
