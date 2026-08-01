#include "camera-stream-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace camera_stream_query;

drogon::Task<std::optional<CameraStreamSchema>>
CameraStreamRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return CameraStreamSchema(result.front());
}

drogon::Task<std::vector<CameraStreamSchema>>
CameraStreamRepository::findByCamera(int64_t cameraId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_CAMERA.data(), cameraId);

  std::vector<CameraStreamSchema> data;
  for (const auto& row : result)
    data.push_back(CameraStreamSchema(row));
  co_return data;
}

drogon::Task<CameraStreamSchema>
CameraStreamRepository::create(const CameraStreamCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(), input.cameraId, input.label,
                                   input.url, input.resolution, input.fps,
                                   input.codec, input.isPrimary ? 1 : 0,
                                   input.isEnabled ? 1 : 0);

  CameraStreamSchema schema;
  schema.id = result.insertId();
  schema.cameraId = input.cameraId;
  schema.label = input.label;
  schema.url = input.url;
  schema.resolution = input.resolution;
  schema.fps = input.fps;
  schema.codec = input.codec;
  schema.isPrimary = input.isPrimary;
  schema.isEnabled = input.isEnabled;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<CameraStreamSchema>
CameraStreamRepository::update(int64_t id,
                               const CameraStreamUpdateInput& input) const
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

  addString(UPDATE_COL_LABEL, input.label);
  addString(UPDATE_COL_URL, input.url);
  addString(UPDATE_COL_RESOLUTION, input.resolution);
  if (input.fps) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_FPS;
    args.push_back(std::to_string(*input.fps));
  }
  addString(UPDATE_COL_CODEC, input.codec);
  if (input.isPrimary) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_IS_PRIMARY;
    args.push_back(*input.isPrimary ? "1" : "0");
  }
  if (input.isEnabled) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_IS_ENABLED;
    args.push_back(*input.isEnabled ? "1" : "0");
  }

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "CameraStream not found for update";
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
    LOG_WARN << "CameraStream not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> CameraStreamRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
CameraStreamRepository::find(const SyncFilter& filter) const
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
    data.push_back(CameraStreamSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
CameraStreamRepository::findDeleted(const SyncFilter& filter) const
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
    data.push_back(CameraStreamSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
CameraStreamRepository::findLast() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST.data());
  if (result.empty())
    co_return std::nullopt;
  co_return CameraStreamSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
CameraStreamRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty())
    co_return std::nullopt;
  co_return CameraStreamSchema(result.front()).toJson();
}
