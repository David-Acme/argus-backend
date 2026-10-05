#include "camera-repository.hxx"

#include <camera/camera-errors.hxx>
#include <ctime>
#include <errors/response-exception.hxx>
#include <shared/repositories/tombstone-page.hxx>
#include <shared/services/secret-box/secret-box.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace camera_query;

namespace
{
std::string sealOrRefuse(const secret_box::SealInput& input)
{
  auto sealed = secret_box::seal(input);
  if (!sealed)
    throw ResponseException(CameraErrors::SecretNotSealed);
  return std::move(*sealed);
}

std::string sealPassword(const std::string& plain)
{
  return sealOrRefuse({.plain = plain, .label = camera_secret::kPasswordLabel});
}

std::string sealCloudPassword(const std::string& plain)
{
  return sealOrRefuse({.plain = plain, .label = camera_secret::kCloudPasswordLabel});
}

bool unreadable(const std::string& stored, const char* label)
{
  return secret_box::isSealed(stored) && !secret_box::open({.stored = stored, .label = label});
}
}

drogon::Task<std::optional<CameraSchema>>
CameraRepository::findById(int64_t id, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::cameraClient();
  auto* effective = client ? client : pooled.get();
  const auto result = co_await effective->execSqlCoro(FIND_BY_ID.data(), id);

  if (result.empty())
    co_return std::nullopt;

  co_return CameraSchema(result.front());
}

drogon::Task<std::vector<CameraSchema>>
CameraRepository::findEnabled() const
{
  auto client = DbService::cameraClient();
  const auto result = co_await client->execSqlCoro(FIND_ENABLED.data());

  std::vector<CameraSchema> data;
  data.reserve(result.size());
  for (const auto& row : result)
    data.push_back(CameraSchema(row));
  co_return data;
}

drogon::Task<std::vector<CameraSchema>> CameraRepository::findLive() const
{
  const auto client = DbService::cameraClient();
  const auto result = co_await client->execSqlCoro(std::string(FIND_LIVE));

  std::vector<CameraSchema> data;
  data.reserve(result.size());
  for (const auto& row : result)
    data.emplace_back(row);
  co_return data;
}

drogon::Task<CameraSchema>
CameraRepository::create(const CameraCreateInput& input) const
{
  const auto pooled = DbService::cameraClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(), input.name,
                                   input.manufacturer, input.model, input.ip,
                                   input.port, input.username,
                                   sealPassword(input.password), input.cloudUsername,
                                   sealCloudPassword(input.cloudPassword),
                                   cameraDriverToString(input.driver), input.icon,
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
  schema.cloudUsername = input.cloudUsername;
  schema.cloudPassword = input.cloudPassword;
  schema.driver = input.driver;
  schema.icon = input.icon;
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
  const auto pooled = DbService::cameraClient();
  auto* client = input.client ? input.client : pooled.get();
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
  addField(UPDATE_COL_PASSWORD,
           input.password ? std::optional(sealPassword(*input.password)) : std::nullopt);
  addField(UPDATE_COL_CLOUD_USERNAME, input.cloudUsername);
  addField(UPDATE_COL_CLOUD_PASSWORD,
           input.cloudPassword ? std::optional(sealCloudPassword(*input.cloudPassword))
                               : std::nullopt);
  if (input.driver) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_DRIVER;
    args.push_back(cameraDriverToString(*input.driver));
  }
  addField(UPDATE_COL_ICON, input.icon);
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

  if (input.resetTapoTrust && !args.empty()) {
    sql += ", ";
    sql += UPDATE_COL_RESET_TRUST;
  }

  if (args.empty()) {
    auto existing = co_await findById(id, client);
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

  auto updated = co_await findById(id, client);
  if (!updated) {
    LOG_WARN << "Camera not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool>
CameraRepository::remove(int64_t id, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::cameraClient();
  auto* effective = client ? client : pooled.get();
  const auto result = co_await effective->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
CameraRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::cameraClient();

  const auto [query, args] =
      sync_query::buildSyncQuery({.filter = filter,
                                  .queryBoth = FIND,
                                  .queryFrom = FIND_FROM,
                                  .queryAll = FIND_ALL,
                                  .queryAfterBoth = FIND_AFTER,
                                  .queryAfterFrom = FIND_AFTER_FROM});
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
  auto client = DbService::cameraClient();

  const auto [query, args] =
      sync_query::buildSyncQuery({.filter = filter,
                                  .queryBoth = FIND_DELETED,
                                  .queryFrom = FIND_DELETED_FROM,
                                  .queryAll = FIND_DELETED_ALL,
                                  .queryAfterBoth = FIND_DELETED_AFTER,
                                  .queryAfterFrom = FIND_DELETED_AFTER_FROM});
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  data.reserve(rows.size());
  for (const auto& row : rows)
    data.push_back(CameraSchema(row).toJson());
  if (!tombstone_page::rereadsBoundary(filter))
    co_return data;
  const auto boundaryRows = co_await client->execSqlCoro(
      std::string(FIND_DELETED_BOUNDARY), filter.startTime.value_or(0),
      filter.startId.value_or(0));
  std::vector<Json::Value> boundary;
  boundary.reserve(boundaryRows.size());
  for (const auto& row : boundaryRows)
    boundary.push_back(CameraSchema(row).toJson());
  co_return tombstone_page::merge({.boundary = std::move(boundary), .page = std::move(data)});
}

drogon::Task<std::optional<Json::Value>> CameraRepository::findLast(const SyncFilter&) const
{
  auto client = DbService::cameraClient();
  const auto result = co_await client->execSqlCoro(FIND_LAST.data());
  if (result.empty())
    co_return std::nullopt;
  co_return CameraSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
CameraRepository::findLastDeleted(const SyncFilter&) const
{
  auto client = DbService::cameraClient();
  const auto result = co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty())
    co_return std::nullopt;
  co_return CameraSchema(result.front()).toJson();
}

bool CameraRepository::acceptTapoTrust()
{
  const auto client = DbService::cameraClient();
  bool fingerprint = false;
  bool secure = false;
  for (const auto& row : client->execSqlSync(std::string(TABLE_COLUMNS))) {
    const auto name = row["name"].as<std::string>();
    fingerprint = fingerprint || name == "tls_fingerprint";
    secure = secure || name == "tapo_secure";
  }
  if (!fingerprint)
    client->execSqlSync(std::string(ADD_TLS_FINGERPRINT));
  if (!secure)
    client->execSqlSync(std::string(ADD_TAPO_SECURE));
  if (!fingerprint || !secure)
    LOG_INFO << "Camera schema: camera rows carry their Tapo certificate pin";
  return true;
}

int64_t CameraRepository::sealPlaintextSecrets()
{
  if (!secret_box::hasKey())
    return 0;
  const auto client = DbService::cameraClient();
  int64_t sealed = 0;
  for (const auto& row : client->execSqlSync(std::string(PLAINTEXT_SECRETS))) {
    const auto reseal = [](const std::string& stored, const char* label) {
      return secret_box::isSealed(stored)
                 ? std::optional<std::string>(stored)
                 : secret_box::seal({.plain = stored, .label = label});
    };
    const auto id = row["id"].as<int64_t>();
    const auto password = reseal(row["password"].as<std::string>(), camera_secret::kPasswordLabel);
    const auto cloudPassword =
        reseal(row["cloud_password"].as<std::string>(), camera_secret::kCloudPasswordLabel);
    if (!password || !cloudPassword) {
      LOG_WARN << "Camera secrets: camera " << id << " stays unencrypted until the next boot";
      continue;
    }
    client->execSqlSync(std::string(SEAL_SECRETS), *password, *cloudPassword, id,
                        row["password"].as<std::string>(),
                        row["cloud_password"].as<std::string>());
    ++sealed;
  }
  return sealed;
}

int64_t CameraRepository::unreadableSecrets()
{
  int64_t unreadableRows = 0;
  for (const auto& row :
       DbService::cameraClient()->execSqlSync(std::string(SEALED_SECRETS))) {
    if (unreadable(row["password"].as<std::string>(), camera_secret::kPasswordLabel) ||
        unreadable(row["cloud_password"].as<std::string>(), camera_secret::kCloudPasswordLabel))
      ++unreadableRows;
  }
  return unreadableRows;
}

void CameraRepository::saveTapoTrust(const CameraTapoTrustInput& input)
{
  if (input.cameraId <= 0 || input.ip.empty())
    return;
  DbService::cameraClient()->execSqlAsync(
      std::string(SAVE_TAPO_TRUST),
      [](const drogon::orm::Result&) {},
      [cameraId = input.cameraId](const drogon::orm::DrogonDbException& error) {
        LOG_WARN << "Camera " << cameraId << ": Tapo trust not saved ("
                 << error.base().what() << ")";
      },
      input.fingerprint, input.secure ? 1 : 0, input.cameraId, input.ip, input.fingerprint);
}
