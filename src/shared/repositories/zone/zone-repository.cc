#include "zone-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace zone_query;

drogon::Task<std::optional<ZoneSchema>>
ZoneRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return ZoneSchema(result.front());
}

drogon::Task<std::vector<ZoneSchema>>
ZoneRepository::findByCamera(int64_t cameraId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_CAMERA.data(), cameraId);

  std::vector<ZoneSchema> data;
  for (const auto& row : result)
    data.push_back(ZoneSchema(row));
  co_return data;
}

drogon::Task<ZoneSchema>
ZoneRepository::create(const ZoneCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(), input.cameraId, input.name,
                                   input.points,
                                   zoneTypeToString(input.zoneType),
                                   input.color, input.isEnabled ? 1 : 0);

  ZoneSchema schema;
  schema.id = result.insertId();
  schema.cameraId = input.cameraId;
  schema.name = input.name;
  schema.points = input.points;
  schema.zoneType = input.zoneType;
  schema.color = input.color;
  schema.isEnabled = input.isEnabled;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<ZoneSchema>
ZoneRepository::update(int64_t id, const ZoneUpdateInput& input) const
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

  addString(UPDATE_COL_NAME, input.name);
  addString(UPDATE_COL_POINTS, input.points);
  if (input.zoneType) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_ZONE_TYPE;
    args.push_back(zoneTypeToString(*input.zoneType));
  }
  addString(UPDATE_COL_COLOR, input.color);
  if (input.isEnabled) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_IS_ENABLED;
    args.push_back(*input.isEnabled ? "1" : "0");
  }

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "Zone not found for update";
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
    LOG_WARN << "Zone not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> ZoneRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
ZoneRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();
  auto result = [&]() -> drogon::Task<drogon::orm::Result> {
    if (filter.startTime && filter.endTime)
      co_return co_await client->execSqlCoro(FIND.data(), *filter.startTime,
                                             *filter.endTime);
    if (filter.startTime)
      co_return co_await client->execSqlCoro(FIND_FROM.data(),
                                             *filter.startTime);
    co_return co_await client->execSqlCoro(FIND_ALL.data());
  }();

  std::vector<Json::Value> data;
  for (const auto& row : co_await result)
    data.push_back(ZoneSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
ZoneRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::client();
  auto result = [&]() -> drogon::Task<drogon::orm::Result> {
    if (filter.startTime && filter.endTime)
      co_return co_await client->execSqlCoro(FIND_DELETED.data(),
                                             *filter.startTime,
                                             *filter.endTime);
    if (filter.startTime)
      co_return co_await client->execSqlCoro(FIND_DELETED_FROM.data(),
                                             *filter.startTime);
    co_return co_await client->execSqlCoro(FIND_DELETED_ALL.data());
  }();

  std::vector<Json::Value> data;
  for (const auto& row : co_await result)
    data.push_back(ZoneSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>> ZoneRepository::findLast() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST.data());
  if (result.empty())
    co_return std::nullopt;
  co_return ZoneSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>> ZoneRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty())
    co_return std::nullopt;
  co_return ZoneSchema(result.front()).toJson();
}
