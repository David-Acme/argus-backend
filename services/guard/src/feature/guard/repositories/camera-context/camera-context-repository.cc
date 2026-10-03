#include "camera-context-repository.hxx"

#include <drogon/orm/Result.h>
#include <sqlite/db-service.hxx>
#include <stdexcept>
#include <string>

using namespace camera_context_query;

namespace
{
GuardCameraContext fromRow(const drogon::orm::Row& row)
{
  return {.cameraId = row["camera_id"].as<int64_t>(),
          .role = cameraRoleFromString(row["role"].as<std::string>())
                      .value_or(CameraRole::Other),
          .outdoor = row["outdoor"].as<int>() != 0,
          .publicArea = row["public_area"].as<int>() != 0,
          .activeHours = row["active_hours"].as<std::string>(),
          .configured = true,
          .updatedAt = row["updated_at"].as<int64_t>()};
}
}

drogon::Task<GuardCameraContext>
CameraContextRepository::find(int64_t cameraId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(SELECT_CONTEXT), cameraId);
  if (rows.empty())
    co_return GuardCameraContext{.cameraId = cameraId,
                                 .role = CameraRole::Other,
                                 .outdoor = false,
                                 .publicArea = false,
                                 .activeHours = {},
                                 .configured = false,
                                 .updatedAt = 0};
  co_return fromRow(rows.front());
}

drogon::Task<std::vector<GuardCameraContext>>
CameraContextRepository::list() const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(LIST_CONTEXTS));
  std::vector<GuardCameraContext> contexts;
  contexts.reserve(rows.size());
  for (const auto& row : rows)
    contexts.push_back(fromRow(row));
  co_return contexts;
}

drogon::Task<GuardCameraContext>
CameraContextRepository::upsert(const CameraContextUpsertInput& input) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(
      std::string(UPSERT_CONTEXT), input.cameraId,
      cameraRoleToString(input.role), input.outdoor ? 1 : 0,
      input.publicArea ? 1 : 0, input.activeHours, input.updatedAt);
  if (rows.empty())
    throw std::runtime_error("camera context upsert returned no row");
  co_return fromRow(rows.front());
}
