#include "notification-repository.hxx"

#include <config/app-config.hxx>
#include <algorithm>
#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <stdexcept>
#include <string>
#include <vector>

using namespace notification_query;

drogon::Task<NotificationSchema>
NotificationRepository::create(const NotificationCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.userId, input.type, input.title, input.body,
      json_util::toString(input.data));

  NotificationSchema schema;
  schema.id = result.insertId();
  schema.userId = input.userId;
  schema.type = input.type;
  schema.title = input.title;
  schema.body = input.body;
  schema.data = input.data;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::vector<NotificationSchema>>
NotificationRepository::createMany(
    const std::vector<NotificationCreateInput>& inputs) const
{
  std::vector<NotificationSchema> schemas;
  if (inputs.empty())
    co_return schemas;

  std::string sql{INSERT_MANY_PREFIX};
  std::vector<std::string> args;
  args.reserve(inputs.size() * 5);
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    if (i > 0)
      sql += ", ";
    sql += "(?, ?, ?, ?, ?)";
    const auto& input = inputs[i];
    args.push_back(std::to_string(input.userId));
    args.push_back(input.type);
    args.push_back(input.title);
    args.push_back(input.body);
    args.push_back(json_util::toString(input.data));
  }
  sql += INSERT_MANY_SUFFIX;

  auto client = DbService::client();
  const auto& argsRef = args;
  const auto result = co_await client->execSqlCoro(sql, argsRef);

  std::vector<int64_t> ids;
  ids.reserve(result.size());
  for (const auto& row : result)
    ids.push_back(row["id"].as<int64_t>());
  if (ids.size() != inputs.size())
    throw std::runtime_error("notification batch insert returned wrong size");
  std::sort(ids.begin(), ids.end());

  const auto now = std::time(nullptr);
  schemas.reserve(inputs.size());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    NotificationSchema schema;
    schema.id = ids[i];
    schema.userId = inputs[i].userId;
    schema.type = inputs[i].type;
    schema.title = inputs[i].title;
    schema.body = inputs[i].body;
    schema.data = inputs[i].data;
    schema.createdAt = now;
    schemas.push_back(std::move(schema));
  }
  co_return schemas;
}

drogon::Task<std::vector<Json::Value>>
NotificationRepository::findSync(const NotificationSyncFilter& filter) const
{
  auto client = DbService::client();
  if (filter.startTime && filter.startId && filter.endTime) {
    const auto result = co_await client->execSqlCoro(
        std::string(FIND_SYNC_AFTER) + AppConfig::SYNC_LIMIT, filter.userId,
        *filter.startTime, *filter.startTime, *filter.startId, *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  if (filter.startTime && filter.startId) {
    const auto result = co_await client->execSqlCoro(
        std::string(FIND_SYNC_AFTER_FROM) + AppConfig::SYNC_LIMIT, filter.userId,
        *filter.startTime, *filter.startTime, *filter.startId);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  if (filter.startTime && filter.endTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC) + AppConfig::SYNC_LIMIT, filter.userId,
                                     *filter.startTime, *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  if (filter.startTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC_FROM) + AppConfig::SYNC_LIMIT, filter.userId,
                                     *filter.startTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  if (filter.endTime) {
    const auto result = co_await client->execSqlCoro(
        std::string(FIND_SYNC_TO) + AppConfig::SYNC_LIMIT, filter.userId, *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  {
    const auto result = co_await client->execSqlCoro(std::string(FIND_SYNC_ALL) + AppConfig::SYNC_LIMIT,
                                                     filter.userId);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
}

drogon::Task<std::optional<Json::Value>>
NotificationRepository::findLastSync(const NotificationSyncFilter& filter) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_SYNC.data(), filter.userId);
  if (result.empty())
    co_return std::nullopt;
  co_return NotificationSchema(result.front()).toJson();
}

drogon::Task<std::vector<NotificationReadChange>>
NotificationRepository::markAsRead(int64_t userId,
                                   const std::vector<int64_t>& ids) const
{
  if (ids.empty())
    co_return {};

  std::string placeholders;
  std::vector<std::string> args;
  args.reserve(ids.size() + 1);
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i > 0)
      placeholders += ", ";
    placeholders += '?';
    args.push_back(std::to_string(ids[i]));
  }
  args.insert(args.begin(), std::to_string(userId));

  const auto withIds = [&placeholders](std::string_view templateQuery) {
    std::string query{templateQuery};
    const auto position = query.find("%1%");
    if (position != std::string::npos)
      query.replace(position, 3, placeholders);
    return query;
  };

  auto client = DbService::client();
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(withIds(FIND_UNREAD_BY_IDS), argsRef);
  if (rows.empty())
    co_return {};

  std::vector<NotificationReadChange> changes;
  changes.reserve(rows.size());
  const auto readAt = static_cast<int64_t>(std::time(nullptr));
  for (const auto& row : rows) {
    NotificationReadChange change;
    change.before = NotificationSchema(row);
    change.after = change.before;
    change.after.isRead = true;
    change.after.readAt = readAt;
    changes.push_back(std::move(change));
  }

  co_await client->execSqlCoro(withIds(MARK_READ), argsRef);
  co_return changes;
}
