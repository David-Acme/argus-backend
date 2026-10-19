#include "camera-patrol-repository.hxx"

#include <sqlite/db-service.hxx>

#include <ctime>
#include <string>
#include <string_view>

using namespace camera_patrol_query;

drogon::Task<std::optional<CameraPatrolSchema>>
CameraPatrolRepository::findByCamera(int64_t cameraId, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::cameraClient();
  auto* target = client != nullptr ? client : pooled.get();
  const auto rows = co_await target->execSqlCoro(std::string(FIND_BY_CAMERA), cameraId);
  if (rows.empty())
    co_return std::nullopt;
  co_return CameraPatrolSchema(rows.front());
}

drogon::Task<std::vector<ActiveCameraPatrol>> CameraPatrolRepository::findActive() const
{
  const auto client = DbService::cameraClient();
  const auto rows = co_await client->execSqlCoro(std::string(FIND_ACTIVE));
  std::vector<ActiveCameraPatrol> patrols;
  patrols.reserve(rows.size());
  for (const auto& row : rows)
    patrols.emplace_back(row);
  co_return patrols;
}

drogon::Task<CameraPatrolSchema> CameraPatrolRepository::create(const CameraPatrolCreateInput& input) const
{
  const auto pooled = DbService::cameraClient();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  const std::string stops = CameraPatrolSchema::stopsText(input.stops);
  const std::optional<int64_t> startHour =
      input.window ? std::optional<int64_t>(input.window->startHour) : std::nullopt;
  const std::optional<int64_t> endHour =
      input.window ? std::optional<int64_t>(input.window->endHour) : std::nullopt;
  const int64_t enabled = input.isEnabled ? 1 : 0;
  const auto result = co_await client->execSqlCoro(std::string(INSERT), input.cameraId, stops,
                                                   input.dwellSeconds, enabled, startHour, endHour);

  CameraPatrolSchema schema;
  schema.cameraId = static_cast<int64_t>(result.insertId());
  schema.stops = input.stops;
  schema.dwellSeconds = input.dwellSeconds;
  schema.isEnabled = input.isEnabled;
  schema.window = input.window;
  schema.createdAt = static_cast<int64_t>(std::time(nullptr));
  co_return schema;
}

drogon::Task<std::optional<CameraPatrolSchema>>
CameraPatrolRepository::update(int64_t cameraId, const CameraPatrolUpdateInput& input) const
{
  const auto pooled = DbService::cameraClient();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  std::string sql(UPDATE_PREFIX);
  std::vector<std::string> args;
  bool changed = false;

  const auto column = [&sql, &changed](std::string_view fragment) {
    if (changed)
      sql += ", ";
    sql += fragment;
    changed = true;
  };

  if (input.stops) {
    column(UPDATE_COL_STOPS);
    args.push_back(CameraPatrolSchema::stopsText(*input.stops));
  }
  if (input.dwellSeconds) {
    column(UPDATE_COL_DWELL_SECONDS);
    args.push_back(std::to_string(*input.dwellSeconds));
  }
  if (input.isEnabled) {
    column(UPDATE_COL_IS_ENABLED);
    args.emplace_back(*input.isEnabled ? "1" : "0");
  }
  if (input.window) {
    if (const auto& window = *input.window) {
      column(UPDATE_COL_WINDOW);
      args.push_back(std::to_string(window->startHour));
      args.push_back(std::to_string(window->endHour));
    }
    else {
      column(UPDATE_COL_NO_WINDOW);
    }
  }

  if (!changed)
    co_return co_await findByCamera(cameraId, client);

  sql += UPDATE_SUFFIX;
  args.push_back(std::to_string(cameraId));
  const auto& argsRef = args;
  co_await client->execSqlCoro(sql, argsRef);
  co_return co_await findByCamera(cameraId, client);
}

drogon::Task<bool> CameraPatrolRepository::remove(int64_t cameraId, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::cameraClient();
  auto* target = client != nullptr ? client : pooled.get();
  const auto result = co_await target->execSqlCoro(std::string(REMOVE), cameraId);
  co_return result.affectedRows() > 0;
}
