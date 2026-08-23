#include "portrait-preview-capability-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace portrait_preview_capability_query;

drogon::Task<PortraitPreviewCapabilitySchema>
PortraitPreviewCapabilityRepository::create(
    const PortraitPreviewCapabilityCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.tokenHash, input.portraitUserId, input.requesterUserId,
      input.expiresAt);
  PortraitPreviewCapabilitySchema schema;
  schema.id = result.insertId();
  schema.tokenHash = input.tokenHash;
  schema.portraitUserId = input.portraitUserId;
  schema.requesterUserId = input.requesterUserId;
  schema.expiresAt = input.expiresAt;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<PortraitPreviewCapabilitySchema>>
PortraitPreviewCapabilityRepository::findByTokenHash(
    const std::string& tokenHash) const
{
  auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(FIND_BY_TOKEN_HASH.data(), tokenHash);
  if (rows.empty())
    co_return std::nullopt;
  co_return PortraitPreviewCapabilitySchema(rows.front());
}

drogon::Task<bool> PortraitPreviewCapabilityRepository::tryConsume(
    const PortraitPreviewCapabilityConsumeInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      TRY_CONSUME.data(), input.id, input.requesterUserId, input.now);
  co_return result.affectedRows() == 1;
}
