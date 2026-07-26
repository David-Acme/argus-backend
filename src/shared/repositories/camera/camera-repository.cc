#include "camera-repository.hxx"
#include <shared/services/sqlite/db-service.hxx>

using namespace camera_query;

drogon::Task<std::optional<CameraSchema>>
CameraRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_ID.data(), id);

  if (result.empty())
    co_return std::nullopt;

  co_return CameraSchema(result.front());
}

drogon::Task<CameraSchema>
CameraRepository::create(const CameraCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.name, input.manufacturer, input.model, input.ip,
      input.port, input.username, input.password,
      cameraRecordModeToString(input.recordMode),
      input.retentionDays ? *input.retentionDays
                          : std::optional<int64_t>{},
      input.capabilities, input.config, 1);

  auto created = co_await findById(result.insertId());
  if (!created) {
    LOG_WARN << "Camera not found after insert";
    co_return {};
  }
  co_return *created;
}

drogon::Task<CameraSchema>
CameraRepository::update(int64_t id, const CameraUpdateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(
      UPDATE.data(), input.name, input.manufacturer, input.model, input.ip,
      input.port, input.username, input.password,
      cameraRecordModeToString(input.recordMode),
      input.retentionDays ? *input.retentionDays
                          : std::optional<int64_t>{},
      input.capabilities, input.config, input.isEnabled ? 1 : 0,
      input.isOnline ? 1 : 0, id);

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
    data.push_back(CameraSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
CameraRepository::findDeleted(const SyncFilter& filter) const
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
    data.push_back(CameraSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
CameraRepository::findLast() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_LAST.data());
  if (result.empty()) co_return std::nullopt;
  co_return CameraSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
CameraRepository::findLastDeleted() const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_DELETED.data());
  if (result.empty()) co_return std::nullopt;
  co_return CameraSchema(result.front()).toJson();
}
