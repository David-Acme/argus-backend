#include "clip-preview-capability-repository.hxx"

#include <sqlite/db-service.hxx>

#include <ctime>

using namespace clip_preview_capability_query;

drogon::Task<ClipPreviewCapabilitySchema>
ClipPreviewCapabilityRepository::create(
    const ClipPreviewCapabilityCreateInput& input) const
{
  const auto pooled = DbService::cameraClient();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(std::string(INSERT), input.tokenHash,
                                                   input.clipId,
                                                   input.requesterUserId,
                                                   input.expiresAt);
  ClipPreviewCapabilitySchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.tokenHash = input.tokenHash;
  schema.clipId = input.clipId;
  schema.requesterUserId = input.requesterUserId;
  schema.expiresAt = input.expiresAt;
  schema.createdAt = static_cast<int64_t>(std::time(nullptr));
  co_return schema;
}

drogon::Task<std::optional<ClipPreviewCapabilitySchema>>
ClipPreviewCapabilityRepository::findByTokenHash(const std::string& tokenHash,
                                                 drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::cameraClient();
  auto* target = client != nullptr ? client : pooled.get();
  const auto rows =
      co_await target->execSqlCoro(std::string(FIND_BY_TOKEN_HASH), tokenHash);
  if (rows.empty())
    co_return std::nullopt;
  co_return ClipPreviewCapabilitySchema(rows.front());
}

drogon::Task<bool> ClipPreviewCapabilityRepository::tryConsume(
    const ClipPreviewCapabilityConsumeInput& input) const
{
  const auto pooled = DbService::cameraClient();
  auto* client = input.client != nullptr ? input.client : pooled.get();
  const auto rows = co_await client->execSqlCoro(
      std::string(TRY_CONSUME), input.id, input.requesterUserId, input.now);
  co_return !rows.empty();
}

drogon::Task<void> ClipPreviewCapabilityRepository::purgeSpent(int64_t now) const
{
  static_cast<void>(
      co_await DbService::cameraClient()->execSqlCoro(std::string(PURGE_SPENT), now));
  co_return;
}
