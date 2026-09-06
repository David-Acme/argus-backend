#include "calendar-event-share-repository.hxx"

#include <shared/services/sqlite/db-service.hxx>
#include <trantor/utils/Logger.h>

using namespace calendar_event_share_query;

drogon::Task<std::optional<CalendarEventShareSchema>>
CalendarEventShareRepository::findById(int64_t id) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return CalendarEventShareSchema(result.front());
}

drogon::Task<std::vector<CalendarEventShareSchema>>
CalendarEventShareRepository::findByParent(int64_t parentId) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(FIND_BY_PARENT.data(), parentId);
  std::vector<CalendarEventShareSchema> rows;
  for (const auto& row : result)
    rows.emplace_back(row);
  co_return rows;
}

drogon::Task<std::optional<ShareAccess>>
CalendarEventShareRepository::findAccess(int64_t parentId, int64_t userId) const
{
  auto client = DbService::productivityClient();
  const auto result =
      co_await client->execSqlCoro(FIND_ACCESS.data(), parentId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return shareAccessFromString(result.front()["access"].as<std::string>());
}

drogon::Task<std::vector<int64_t>>
CalendarEventShareRepository::memberIds(int64_t parentId) const
{
  auto client = DbService::productivityClient();
  const auto result =
      co_await client->execSqlCoro(FIND_MEMBER_IDS.data(), parentId);
  std::vector<int64_t> ids;
  ids.reserve(result.size());
  for (const auto& row : result)
    ids.push_back(static_cast<int64_t>(row["user_id"].as<long long>()));
  co_return ids;
}

drogon::Task<std::optional<CalendarEventShareSchema>>
CalendarEventShareRepository::findExisting(int64_t parentId, int64_t userId) const
{
  auto client = DbService::productivityClient();
  const auto result =
      co_await client->execSqlCoro(FIND_EXISTING.data(), parentId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return CalendarEventShareSchema(result.front());
}

drogon::Task<CalendarEventShareSchema>
CalendarEventShareRepository::create(const CalendarEventShareCreateInput& input) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.calendarEventId, input.userId,
      shareAccessToString(input.access));
  const auto row = co_await findById(static_cast<int64_t>(result.insertId()));
  if (!row) {
    LOG_WARN << "Membership row vanished right after insert";
    co_return CalendarEventShareSchema{};
  }
  co_return *row;
}

drogon::Task<CalendarEventShareSchema>
CalendarEventShareRepository::updateAccess(int64_t id, ShareAccess access) const
{
  auto client = DbService::productivityClient();
  co_await client->execSqlCoro(UPDATE_ACCESS.data(),
                               shareAccessToString(access), id);
  const auto row = co_await findById(id);
  if (!row)
    co_return CalendarEventShareSchema{};
  co_return *row;
}

drogon::Task<bool> CalendarEventShareRepository::remove(int64_t id) const
{
  auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
CalendarEventShareRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto [query, args] = sync_query::withUser(
      sync_query::buildSyncQuery(filter, FIND, FIND_FROM, FIND_ALL, FIND_AFTER,
                                 FIND_AFTER_FROM),
      filter.userId, OWNERSHIP_PLACEHOLDERS);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(CalendarEventShareSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
CalendarEventShareRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto [query, args] = sync_query::withUser(
      sync_query::buildSyncQuery(filter, FIND_DELETED, FIND_DELETED_FROM,
                                 FIND_DELETED_ALL, FIND_DELETED_AFTER,
                                 FIND_DELETED_AFTER_FROM),
      filter.userId, OWNERSHIP_PLACEHOLDERS);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(CalendarEventShareSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
CalendarEventShareRepository::findLast(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return CalendarEventShareSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
CalendarEventShareRepository::findLastDeleted(const SyncFilter& filter) const
{
  auto client = DbService::productivityClient();
  const auto userId = filter.userId.value_or(0);
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_DELETED.data(), userId, userId);
  if (result.empty())
    co_return std::nullopt;
  co_return CalendarEventShareSchema(result.front()).toJson();
}
