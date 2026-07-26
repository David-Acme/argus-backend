#include "camera-stream-repository.hxx"
#include <shared/services/sqlite/db-service.hxx>

using namespace camera_stream_query;

drogon::Task<std::optional<CameraStreamSchema>>
CameraStreamRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty()) co_return std::nullopt;
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
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.cameraId, input.label, input.url,
      input.resolution, input.fps, input.codec,
      input.isPrimary ? 1 : 0, input.isEnabled ? 1 : 0);

  auto created = co_await findById(result.insertId());
  if (!created) {
    LOG_WARN << "CameraStream not found after insert";
    co_return {};
  }
  co_return *created;
}

drogon::Task<CameraStreamSchema>
CameraStreamRepository::update(int64_t id,
                               const CameraStreamUpdateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(
      UPDATE.data(), input.label, input.url, input.resolution, input.fps,
      input.codec, input.isPrimary ? 1 : 0, input.isEnabled ? 1 : 0, id);

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
      co_return co_await client->execSqlCoro(
          FIND_DELETED.data(), *filter.startTime, *filter.endTime);
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
  if (result.empty()) co_return std::nullopt;
  co_return CameraStreamSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
CameraStreamRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty()) co_return std::nullopt;
  co_return CameraStreamSchema(result.front()).toJson();
}
