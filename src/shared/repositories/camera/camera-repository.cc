#include "camera-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace camera_query;

drogon::Task<std::optional<CameraSchema>>
CameraRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);

  if (result.empty())
    co_return std::nullopt;

  co_return CameraSchema(result.front());
}

drogon::Task<CameraSchema>
CameraRepository::create(const CameraCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(), input.name,
                                   input.manufacturer, input.model, input.ip,
                                   input.port, input.username, input.password,
                                   cameraRecordModeToString(input.recordMode),
                                   input.retentionDays
                                       ? *input.retentionDays
                                       : std::optional<int64_t>{},
                                   input.capabilities, input.config, 1);

  CameraSchema schema;
  schema.id = result.insertId();
  schema.name = input.name;
  schema.manufacturer = input.manufacturer;
  schema.model = input.model;
  schema.ip = input.ip;
  schema.port = input.port;
  schema.username = input.username;
  schema.password = input.password;
  schema.recordMode = input.recordMode;
  schema.retentionDays = input.retentionDays;
  schema.capabilities = input.capabilities;
  schema.config = input.config;
  schema.isEnabled = true;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<CameraSchema>
CameraRepository::update(int64_t id, const CameraUpdateInput& input) const
{
  auto client = DbService::client();
  std::string sql = UPDATE_PREFIX.data();
  std::vector<std::string> args;

  auto addField = [&](std::string_view column,
                      const std::optional<std::string>& value) {
    if (!value)
      return;
    if (!args.empty())
      sql += ", ";
    sql += column;
    args.push_back(*value);
  };

  addField(UPDATE_COL_NAME, input.name);
  addField(UPDATE_COL_MANUFACTURER, input.manufacturer);
  addField(UPDATE_COL_MODEL, input.model);
  addField(UPDATE_COL_IP, input.ip);
  if (input.port) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_PORT;
    args.push_back(std::to_string(*input.port));
  }
  addField(UPDATE_COL_USERNAME, input.username);
  addField(UPDATE_COL_PASSWORD, input.password);
  if (input.recordMode) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_RECORD_MODE;
    args.push_back(cameraRecordModeToString(*input.recordMode));
  }
  if (input.retentionDays) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_RETENTION_DAYS;
    args.push_back(std::to_string(*input.retentionDays));
  }
  addField(UPDATE_COL_CAPABILITIES, input.capabilities);
  addField(UPDATE_COL_CONFIG, input.config);
  if (input.isEnabled) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_IS_ENABLED;
    args.push_back(*input.isEnabled ? "1" : "0");
  }
  if (input.isOnline) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_IS_ONLINE;
    args.push_back(*input.isOnline ? "1" : "0");
  }

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "Camera not found for update";
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
    LOG_WARN << "Camera not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> CameraRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
CameraRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();

  const auto [query, args] =
      sync_query::buildSyncQuery(filter, FIND, FIND_FROM, FIND_ALL);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(CameraSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
CameraRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::client();

  const auto [query, args] =
      sync_query::buildSyncQuery(filter, FIND_DELETED, FIND_DELETED_FROM, FIND_DELETED_ALL);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(CameraSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>> CameraRepository::findLast() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST.data());
  if (result.empty())
    co_return std::nullopt;
  co_return CameraSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
CameraRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty())
    co_return std::nullopt;
  co_return CameraSchema(result.front()).toJson();
}
