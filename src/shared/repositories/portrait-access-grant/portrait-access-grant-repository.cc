#include "portrait-access-grant-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace portrait_access_grant_query;

drogon::Task<PortraitAccessGrantSchema>
PortraitAccessGrantRepository::create(
    const PortraitAccessGrantCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(),
      input.requestId ? *input.requestId : std::optional<int64_t>{},
      input.portraitUserId, input.granteeUserId, input.grantedBy,
      portraitAccessGrantScopeToString(input.scope),
      input.expiresAt ? *input.expiresAt : std::optional<int64_t>{});

  PortraitAccessGrantSchema schema;
  schema.id = result.insertId();
  schema.requestId = input.requestId;
  schema.portraitUserId = input.portraitUserId;
  schema.granteeUserId = input.granteeUserId;
  schema.grantedBy = input.grantedBy;
  schema.scope = input.scope;
  schema.expiresAt = input.expiresAt;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<PortraitAccessGrantSchema>>
PortraitAccessGrantRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return PortraitAccessGrantSchema(result.front());
}

drogon::Task<std::optional<PortraitAccessGrantSchema>>
PortraitAccessGrantRepository::findActive(
    const PortraitAccessGrantFindActiveInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      FIND_ACTIVE.data(), input.portraitUserId, input.granteeUserId, input.now);
  if (result.empty())
    co_return std::nullopt;
  co_return PortraitAccessGrantSchema(result.front());
}

drogon::Task<std::vector<PortraitAccessGrantSchema>>
PortraitAccessGrantRepository::findActiveForGrantee(
    int64_t granteeUserId, int64_t now) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      FIND_ACTIVE_FOR_GRANTEE.data(), granteeUserId, now);
  std::vector<PortraitAccessGrantSchema> grants;
  grants.reserve(result.size());
  for (const auto& row : result)
    grants.emplace_back(row);
  co_return grants;
}

drogon::Task<bool>
PortraitAccessGrantRepository::revoke(
    const PortraitAccessGrantRevokeInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      REVOKE.data(), input.revokedBy, input.grantId);
  co_return result.affectedRows() > 0;
}
