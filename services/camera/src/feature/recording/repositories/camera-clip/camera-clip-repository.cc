#include "camera-clip-repository.hxx"

#include <ctime>
#include <sqlite/db-service.hxx>

#include <string>
#include <utility>

using namespace camera_clip_query;

drogon::Task<CameraClipSchema>
CameraClipRepository::create(const CameraClipCreateInput& input) const
{
  const auto pooled = DbService::cameraClient();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.cameraId, cameraClipTriggerToString(input.trigger),
      input.objectKey, input.contentType, input.codec, input.startedAt,
      input.durationMs, input.byteSize, input.expiresAt);

  CameraClipSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.cameraId = input.cameraId;
  schema.trigger = input.trigger;
  schema.objectKey = input.objectKey;
  schema.contentType = input.contentType;
  schema.codec = input.codec;
  schema.startedAt = input.startedAt;
  schema.durationMs = input.durationMs;
  schema.byteSize = input.byteSize;
  schema.createdAt = static_cast<int64_t>(std::time(nullptr));
  schema.expiresAt = input.expiresAt;
  co_return schema;
}

drogon::Task<std::optional<CameraClipSchema>>
CameraClipRepository::findById(int64_t id, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::cameraClient();
  auto* target = client != nullptr ? client : pooled.get();
  const auto rows = co_await target->execSqlCoro(FIND_BY_ID.data(), id);
  if (rows.empty())
    co_return std::nullopt;
  co_return CameraClipSchema(rows.front());
}

drogon::Task<std::vector<CameraClipSchema>>
CameraClipRepository::findByCamera(const CameraClipPageInput& input) const
{
  const auto rows = co_await DbService::cameraClient()->execSqlCoro(
      FIND_BY_CAMERA.data(), input.cameraId, input.beforeCreatedAt,
      input.beforeCreatedAt, input.beforeId, input.limit);
  std::vector<CameraClipSchema> clips;
  clips.reserve(rows.size());
  for (const auto& row : rows)
    clips.emplace_back(row);
  co_return clips;
}

drogon::Task<std::vector<CameraClipSchema>>
CameraClipRepository::findByCameraAfter(const CameraClipAfterInput& input) const
{
  const auto rows = co_await DbService::cameraClient()->execSqlCoro(
      FIND_BY_CAMERA_AFTER.data(), input.cameraId, input.sinceCreatedAt,
      input.limit);
  std::vector<CameraClipSchema> clips;
  clips.reserve(rows.size());
  for (const auto& row : rows)
    clips.emplace_back(row);
  co_return clips;
}

drogon::Task<bool>
CameraClipRepository::markDeleted(const CameraClipDeleteInput& input) const
{
  const auto pooled = DbService::cameraClient();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  const auto rows =
      co_await client->execSqlCoro(MARK_DELETED.data(), input.deletedAt, input.id);
  co_return !rows.empty();
}

drogon::Task<std::vector<CameraClipSchema>>
CameraClipRepository::findExpired(const CameraClipSweepInput& input) const
{
  const auto rows = co_await DbService::cameraClient()->execSqlCoro(
      EXPIRED_CLIPS.data(), input.afterId, input.now, input.now,
      input.maxRetentionDays, input.batch);
  std::vector<CameraClipSchema> clips;
  clips.reserve(rows.size());
  for (const auto& row : rows)
    clips.emplace_back(row);
  co_return clips;
}

drogon::Task<int64_t>
CameraClipRepository::countExpired(int64_t now, int64_t maxRetentionDays) const
{
  const auto rows = co_await DbService::cameraClient()->execSqlCoro(
      COUNT_EXPIRED_CLIPS.data(), now, now, maxRetentionDays);
  co_return rows.empty()
                ? 0
                : static_cast<int64_t>(rows.front()["total"].as<int64_t>());
}

drogon::Task<std::optional<int64_t>>
CameraClipRepository::retentionDaysOf(int64_t cameraId,
                                       drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::cameraClient();
  auto* target = client != nullptr ? client : pooled.get();
  const auto rows =
      co_await target->execSqlCoro(CAMERA_CLIP_RETENTION.data(), cameraId);
  if (rows.empty() || rows.front()["retention_days"].isNull())
    co_return std::nullopt;
  co_return static_cast<int64_t>(rows.front()["retention_days"].as<int64_t>());
}

drogon::Task<void> CameraClipRepository::purgeDeleted(int64_t before) const
{
  static_cast<void>(
      co_await DbService::cameraClient()->execSqlCoro(PURGE_DELETED.data(), before));
  co_return;
}
